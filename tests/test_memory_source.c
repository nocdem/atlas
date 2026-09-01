/* Atlas - A12.1: the memory vocabularies, and the policy grammar that names a
 * memory source.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Enumerated tests over closed vocabularies, in the shape
 * `tests/test_verify_model.c` and `tests/test_gate_model.c` use: every member is
 * visited, and every expectation is written out here rather than derived from
 * the same table the implementation reads. A test that agreed with a second copy
 * of the rules would pass while the rules were wrong.
 *
 * Nothing here opens a database, creates a process or reads a repository. The
 * vocabularies are pure functions of an enum, and the policy value grammar is a
 * pure function of a byte range — which is exactly why
 * `atlas_memory_source_value_parse` is exposed: a grammar that could only be
 * exercised through a root-owned file on disk could not be enumerated at all.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "atlas/error.h"
#include "atlas/limits.h"
#include "atlas/memory.h"
#include "atlas/syspolicy.h"
#include "atlas_test.h"
#include "support/fixture.h"

/* --- zeros ---------------------------------------------------------------- */

static void test_unknown_is_zero_in_every_vocabulary(void) {
    /* A6 keeps UNKNOWN at zero, A8 keeps DISABLED there, A9.2 follows, and so
     * does this season: a `memset` must never produce a member that asserts
     * something. Checked as values rather than trusted, because a later edit
     * that reorders an enum would move them silently. */
    T_EQ_INT((int)ATLAS_MEMORY_SOURCE_UNKNOWN, 0);
    T_EQ_INT((int)ATLAS_MEMORY_DIFF_UNKNOWN, 0);
    T_EQ_INT((int)ATLAS_MEMORY_ANCHOR_UNKNOWN, 0);
    T_EQ_INT((int)ATLAS_MEMORY_PACK_UNKNOWN, 0);
    T_EQ_INT((int)ATLAS_MEMORY_CAUSE_UNKNOWN, 0);
}

static void test_the_zero_member_is_named_unknown_and_never_parses(void) {
    /* Both halves matter and they are not the same claim. The *name* is
     * "UNKNOWN" so that a zero which reached a renderer says so instead of
     * naming a member that asserts something. The *parse* refuses that spelling
     * so that a caller cannot store one: zero means "nobody filled this in", and
     * a parser that accepted the word would let a request write the absence of a
     * statement as though it were a statement. */
    T_EQ_STR(atlas_memory_source_class_name(ATLAS_MEMORY_SOURCE_UNKNOWN), "UNKNOWN");
    T_EQ_STR(atlas_memory_diff_kind_name(ATLAS_MEMORY_DIFF_UNKNOWN), "UNKNOWN");
    T_EQ_STR(atlas_memory_anchor_kind_name(ATLAS_MEMORY_ANCHOR_UNKNOWN), "UNKNOWN");
    T_EQ_STR(atlas_memory_pack_status_name(ATLAS_MEMORY_PACK_UNKNOWN), "UNKNOWN");
    T_EQ_STR(atlas_memory_gen_cause_name(ATLAS_MEMORY_CAUSE_UNKNOWN), "UNKNOWN");

    atlas_memory_source_class cls = ATLAS_MEMORY_SOURCE_REPO_FILE;
    atlas_memory_diff_kind diff = ATLAS_MEMORY_DIFF_ADDED;
    atlas_memory_anchor_kind anchor = ATLAS_MEMORY_ANCHOR_PATH;
    atlas_memory_pack_status pack = ATLAS_MEMORY_PACK_CURRENT;
    atlas_memory_gen_cause cause = ATLAS_MEMORY_CAUSE_COMMIT;

    T_CHECK_MSG(!atlas_memory_source_class_parse("UNKNOWN", &cls),
                "the source class parser accepted the zero member's name");
    T_CHECK_MSG(!atlas_memory_diff_kind_parse("UNKNOWN", &diff),
                "the diff kind parser accepted the zero member's name");
    T_CHECK_MSG(!atlas_memory_anchor_kind_parse("UNKNOWN", &anchor),
                "the anchor kind parser accepted the zero member's name");
    T_CHECK_MSG(!atlas_memory_pack_status_parse("UNKNOWN", &pack),
                "the pack status parser accepted the zero member's name");
    T_CHECK_MSG(!atlas_memory_gen_cause_parse("UNKNOWN", &cause),
                "the gen cause parser accepted the zero member's name");

    /* A refused parse leaves the caller's variable alone, so a caller that
     * ignored the return value is not handed a member it never asked for. */
    T_EQ_INT((int)cls, (int)ATLAS_MEMORY_SOURCE_REPO_FILE);
    T_EQ_INT((int)diff, (int)ATLAS_MEMORY_DIFF_ADDED);
    T_EQ_INT((int)anchor, (int)ATLAS_MEMORY_ANCHOR_PATH);
    T_EQ_INT((int)pack, (int)ATLAS_MEMORY_PACK_CURRENT);
    T_EQ_INT((int)cause, (int)ATLAS_MEMORY_CAUSE_COMMIT);
}

/* --- round trips ---------------------------------------------------------- */

static void test_every_source_class_round_trips(void) {
    static const struct {
        atlas_memory_source_class c;
        const char *name;
    } CASES[] = {
        {ATLAS_MEMORY_SOURCE_REPO_FILE, "REPO_FILE"},
        {ATLAS_MEMORY_SOURCE_REPO_DIR, "REPO_DIR"},
        {ATLAS_MEMORY_SOURCE_EXTERNAL_FILE, "EXTERNAL_FILE"},
        {ATLAS_MEMORY_SOURCE_EXTERNAL_DIR, "EXTERNAL_DIR"},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        T_EQ_STR(atlas_memory_source_class_name(CASES[i].c), CASES[i].name);
        atlas_memory_source_class got = ATLAS_MEMORY_SOURCE_UNKNOWN;
        T_CHECK_MSG(atlas_memory_source_class_parse(CASES[i].name, &got), "\"%s\" did not parse",
                    CASES[i].name);
        T_EQ_INT((int)got, (int)CASES[i].c);
        T_EQ_STR(atlas_memory_source_class_name(got), CASES[i].name);
    }
    atlas_memory_source_class got = ATLAS_MEMORY_SOURCE_UNKNOWN;
    T_CHECK(!atlas_memory_source_class_parse("repo_file", &got));
    T_CHECK(!atlas_memory_source_class_parse("", &got));
    T_CHECK(!atlas_memory_source_class_parse(NULL, &got));
}

static void test_every_diff_kind_round_trips(void) {
    static const struct {
        atlas_memory_diff_kind k;
        const char *name;
    } CASES[] = {
        {ATLAS_MEMORY_DIFF_ADDED, "ADDED"},
        {ATLAS_MEMORY_DIFF_CHANGED, "CHANGED"},
        {ATLAS_MEMORY_DIFF_SUPPORTED, "SUPPORTED"},
        {ATLAS_MEMORY_DIFF_CONTRADICTED, "CONTRADICTED"},
        {ATLAS_MEMORY_DIFF_STALE, "STALE"},
        {ATLAS_MEMORY_DIFF_IMPACTED, "IMPACTED"},
        {ATLAS_MEMORY_DIFF_SUPERSEDED, "SUPERSEDED"},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        T_EQ_STR(atlas_memory_diff_kind_name(CASES[i].k), CASES[i].name);
        atlas_memory_diff_kind got = ATLAS_MEMORY_DIFF_UNKNOWN;
        T_CHECK_MSG(atlas_memory_diff_kind_parse(CASES[i].name, &got), "\"%s\" did not parse",
                    CASES[i].name);
        T_EQ_INT((int)got, (int)CASES[i].k);
        T_EQ_STR(atlas_memory_diff_kind_name(got), CASES[i].name);
    }
    atlas_memory_diff_kind got = ATLAS_MEMORY_DIFF_UNKNOWN;
    T_CHECK(!atlas_memory_diff_kind_parse("added", &got));
    T_CHECK(!atlas_memory_diff_kind_parse(NULL, &got));
}

static void test_every_anchor_kind_round_trips(void) {
    static const struct {
        atlas_memory_anchor_kind k;
        const char *name;
    } CASES[] = {
        {ATLAS_MEMORY_ANCHOR_PATH, "PATH"},
        {ATLAS_MEMORY_ANCHOR_SYMBOL, "SYMBOL"},
        {ATLAS_MEMORY_ANCHOR_DECISION, "DECISION"},
        {ATLAS_MEMORY_ANCHOR_COMMIT, "COMMIT"},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        T_EQ_STR(atlas_memory_anchor_kind_name(CASES[i].k), CASES[i].name);
        atlas_memory_anchor_kind got = ATLAS_MEMORY_ANCHOR_UNKNOWN;
        T_CHECK_MSG(atlas_memory_anchor_kind_parse(CASES[i].name, &got), "\"%s\" did not parse",
                    CASES[i].name);
        T_EQ_INT((int)got, (int)CASES[i].k);
        T_EQ_STR(atlas_memory_anchor_kind_name(got), CASES[i].name);
    }
    atlas_memory_anchor_kind got = ATLAS_MEMORY_ANCHOR_UNKNOWN;
    T_CHECK(!atlas_memory_anchor_kind_parse("path", &got));
    T_CHECK(!atlas_memory_anchor_kind_parse(NULL, &got));
}

static void test_every_pack_status_round_trips(void) {
    static const struct {
        atlas_memory_pack_status s;
        const char *name;
    } CASES[] = {
        {ATLAS_MEMORY_PACK_CURRENT, "CURRENT"},
        {ATLAS_MEMORY_PACK_STALE, "STALE"},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        T_EQ_STR(atlas_memory_pack_status_name(CASES[i].s), CASES[i].name);
        atlas_memory_pack_status got = ATLAS_MEMORY_PACK_UNKNOWN;
        T_CHECK_MSG(atlas_memory_pack_status_parse(CASES[i].name, &got), "\"%s\" did not parse",
                    CASES[i].name);
        T_EQ_INT((int)got, (int)CASES[i].s);
        T_EQ_STR(atlas_memory_pack_status_name(got), CASES[i].name);
    }
    atlas_memory_pack_status got = ATLAS_MEMORY_PACK_UNKNOWN;
    T_CHECK(!atlas_memory_pack_status_parse("current", &got));
    T_CHECK(!atlas_memory_pack_status_parse(NULL, &got));
}

static void test_every_gen_cause_round_trips(void) {
    static const struct {
        atlas_memory_gen_cause c;
        const char *name;
    } CASES[] = {
        {ATLAS_MEMORY_CAUSE_SOURCE_REVISION, "SOURCE_REVISION"},
        {ATLAS_MEMORY_CAUSE_DECISION_REVISION, "DECISION_REVISION"},
        {ATLAS_MEMORY_CAUSE_COMMIT, "COMMIT"},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        T_EQ_STR(atlas_memory_gen_cause_name(CASES[i].c), CASES[i].name);
        atlas_memory_gen_cause got = ATLAS_MEMORY_CAUSE_UNKNOWN;
        T_CHECK_MSG(atlas_memory_gen_cause_parse(CASES[i].name, &got), "\"%s\" did not parse",
                    CASES[i].name);
        T_EQ_INT((int)got, (int)CASES[i].c);
        T_EQ_STR(atlas_memory_gen_cause_name(got), CASES[i].name);
    }
    atlas_memory_gen_cause got = ATLAS_MEMORY_CAUSE_UNKNOWN;
    T_CHECK(!atlas_memory_gen_cause_parse("commit", &got));
    T_CHECK(!atlas_memory_gen_cause_parse(NULL, &got));
}

/* --- who reads the bytes -------------------------------------------------- */

static void test_is_repo_is_true_for_exactly_the_two_repo_classes(void) {
    /* The one implementation of "who reads this". A REPO_ class names a path
     * inside a registered repository, which the daemon reaches the way it
     * reaches every other file in that tree; an EXTERNAL_ class names an
     * absolute path outside every repository, which is a different question
     * about a different reader. Enumerated over the whole vocabulary rather than
     * sampled, because the interesting failure is a member added later that
     * nobody classified. */
    T_CHECK(atlas_memory_source_class_is_repo(ATLAS_MEMORY_SOURCE_REPO_FILE));
    T_CHECK(atlas_memory_source_class_is_repo(ATLAS_MEMORY_SOURCE_REPO_DIR));
    T_CHECK(!atlas_memory_source_class_is_repo(ATLAS_MEMORY_SOURCE_EXTERNAL_FILE));
    T_CHECK(!atlas_memory_source_class_is_repo(ATLAS_MEMORY_SOURCE_EXTERNAL_DIR));
    /* A zero is not a repository class. It is not any class. */
    T_CHECK(!atlas_memory_source_class_is_repo(ATLAS_MEMORY_SOURCE_UNKNOWN));
}

/* --- the policy value grammar --------------------------------------------- */

static void test_the_value_grammar_accepts_exactly_what_it_should(void) {
    /* `CLASS[@repo]:path`. Split at the first `:`, split the head at the first
     * `@`. Every field is written out here rather than derived from the parser,
     * because a test that agreed with a second copy of the rules would pass
     * while the rules were wrong. */
    static const struct {
        const char *val;
        atlas_memory_source_class cls;
        const char *repo;
        const char *path;
        const char *what;
    } OK[] = {
        {"REPO_FILE:CLAUDE.md", ATLAS_MEMORY_SOURCE_REPO_FILE, "", "CLAUDE.md",
         "a repository file"},
        {"REPO_DIR:.claude/memories", ATLAS_MEMORY_SOURCE_REPO_DIR, "", ".claude/memories",
         "a repository directory"},
        {"EXTERNAL_FILE:/x/y.md", ATLAS_MEMORY_SOURCE_EXTERNAL_FILE, "", "/x/y.md",
         "an external file"},
        {"EXTERNAL_DIR:/x/mem", ATLAS_MEMORY_SOURCE_EXTERNAL_DIR, "", "/x/mem",
         "an external directory"},
        {"REPO_FILE@atlas:CLAUDE.md", ATLAS_MEMORY_SOURCE_REPO_FILE, "atlas", "CLAUDE.md",
         "a source scoped to one named repository"},
        {"EXTERNAL_DIR@atlas:/x/mem", ATLAS_MEMORY_SOURCE_EXTERNAL_DIR, "atlas", "/x/mem",
         "an external directory scoped to one named repository"},
        /* The `..` rule is about a path *component*, not a substring. This is a
         * perfectly ordinary filename and refusing it would be the obvious bug
         * in a parser that reached for `strstr`. */
        {"REPO_FILE:a..b.md", ATLAS_MEMORY_SOURCE_REPO_FILE, "", "a..b.md",
         "two dots inside a component are part of a filename"},
    };
    for (size_t i = 0; i < sizeof OK / sizeof OK[0]; i++) {
        atlas_syspolicy_memory_source s;
        memset(&s, 0xff, sizeof s);
        T_CHECK_MSG(atlas_memory_source_value_parse(OK[i].val, strlen(OK[i].val), &s),
                    "\"%s\" was refused (%s)", OK[i].val, OK[i].what);
        T_EQ_INT((int)s.cls, (int)OK[i].cls);
        T_EQ_STR(s.repo_name, OK[i].repo);
        T_EQ_STR(s.path, OK[i].path);
    }
}

static void test_the_value_grammar_refuses_rather_than_repairs(void) {
    static const struct {
        const char *val;
        const char *why;
    } BAD[] = {
        {"REPO_FILE", "no colon at all, so nothing names a path"},
        {"FOO:x", "an unrecognised class"},
        {"REPO_FILE:/absolute", "a repository path is relative to the repository root"},
        {"REPO_FILE:a/../b", "a `..` component leaves the repository"},
        {"REPO_FILE:.git/config", "the repository's own metadata is not a memory source"},
        {"EXTERNAL_FILE:relative", "an external path is absolute or it is not external"},
        {"REPO_FILE:", "an empty path"},
        {"REPO_FILE@:CLAUDE.md", "an empty repository name after `@`"},
        {"REPO_FILE@a/b:CLAUDE.md", "a repository name is a registry name, never a path"},
        {"REPO_FILE@..:CLAUDE.md", "a repository name that is a path component"},
        {"", "nothing at all"},
    };
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        atlas_syspolicy_memory_source s;
        memset(&s, 0xff, sizeof s);
        T_CHECK_MSG(!atlas_memory_source_value_parse(BAD[i].val, strlen(BAD[i].val), &s),
                    "\"%s\" was accepted; %s", BAD[i].val, BAD[i].why);
        /* A refusal leaves the zero, not a half-filled source: a caller that
         * ignored the return value must not be handed a class that asserts
         * something. */
        T_EQ_INT((int)s.cls, (int)ATLAS_MEMORY_SOURCE_UNKNOWN);
    }

    /* A path longer than the field is refused, not truncated. Exactly at the
     * field's capacity is accepted, so the boundary is a boundary and not a
     * fence post nobody checked. */
    char val[1024];
    atlas_syspolicy_memory_source s;
    memset(&s, 0, sizeof s);
    size_t fits = sizeof s.path - 1u;
    int n = snprintf(val, sizeof val, "REPO_FILE:");
    T_REQUIRE(n > 0);
    memset(val + n, 'a', fits);
    val[(size_t)n + fits] = '\0';
    T_CHECK_MSG(atlas_memory_source_value_parse(val, strlen(val), &s),
                "a path of exactly the field's capacity was refused");
    T_CHECK(strlen(s.path) == fits);

    memset(&s, 0xff, sizeof s);
    val[(size_t)n + fits] = 'a';
    val[(size_t)n + fits + 1u] = '\0';
    T_CHECK_MSG(!atlas_memory_source_value_parse(val, strlen(val), &s),
                "a path one byte over the field was accepted");
    T_EQ_INT((int)s.cls, (int)ATLAS_MEMORY_SOURCE_UNKNOWN);

    /* And the same for the repository name. */
    memset(&s, 0xff, sizeof s);
    char big[256];
    n = snprintf(big, sizeof big, "REPO_FILE@");
    T_REQUIRE(n > 0);
    memset(big + n, 'r', sizeof s.repo_name);
    (void)snprintf(big + (size_t)n + sizeof s.repo_name,
                   sizeof big - (size_t)n - sizeof s.repo_name, ":CLAUDE.md");
    T_CHECK_MSG(!atlas_memory_source_value_parse(big, strlen(big), &s),
                "a repository name one byte over the field was accepted");
    T_EQ_INT((int)s.cls, (int)ATLAS_MEMORY_SOURCE_UNKNOWN);

    /* A NUL inside the value would silently shorten whatever was stored, which
     * is the one failure this grammar exists to avoid. Refused. */
    memset(&s, 0xff, sizeof s);
    static const char embedded[] = "REPO_FILE:CLA\0UDE.md";
    T_CHECK_MSG(!atlas_memory_source_value_parse(embedded, sizeof embedded - 1u, &s),
                "a value carrying a NUL byte was accepted");
    T_EQ_INT((int)s.cls, (int)ATLAS_MEMORY_SOURCE_UNKNOWN);

    T_CHECK(!atlas_memory_source_value_parse(NULL, 0, &s));
}

/* --- the effective sweep gate ---------------------------------------------- */

static void test_the_sweep_gate_resolves_unset_to_the_named_default(void) {
    atlas_syspolicy p;
    memset(&p, 0, sizeof p);

    /* Zero is UNSET, and UNSET is the absence of a statement rather than a
     * statement of "no". What it resolves to is the named compiled-in constant,
     * so a `memset` produces "the policy says nothing" instead of a value that
     * happens to start a pass. */
    T_EQ_INT((int)p.memory_reconcile, 0);
    T_CHECK(atlas_syspolicy_memory_reconcile_effective(&p) == ATLAS_MEMORY_RECONCILE_DEFAULT);
    T_CHECK(atlas_syspolicy_memory_reconcile_effective(NULL) == ATLAS_MEMORY_RECONCILE_DEFAULT);

    p.memory_reconcile = 1;
    T_CHECK_MSG(atlas_syspolicy_memory_reconcile_effective(&p), "ENABLED did not enable the sweep");
    p.memory_reconcile = 2;
    T_CHECK_MSG(!atlas_syspolicy_memory_reconcile_effective(&p),
                "DISABLED did not disable the sweep");

    /* The compiled-in default is `false`: reading documents an operator named
     * and storing claims from them is not something the absence of a statement
     * consents to. Asserted as a value so a change to it has to be deliberate. */
    T_CHECK_MSG(ATLAS_MEMORY_RECONCILE_DEFAULT == false,
                "the reconcile default changed; that is a decision, not a constant");
}

static void test_a_policy_atlas_could_not_read_registers_nothing(void) {
    /* The parser assigns each key as it reads it and returns on the first
     * malformed line -- `client_uid`'s established shape. So a policy carrying a
     * well-formed `memory_reconcile = ENABLED` and two good `memory_source`
     * lines, followed later by a typo'd key, ends with `state` LEGACY and both
     * fields *populated*. Reading them then would honour half a policy nobody
     * can read back, and the direction is the dangerous one: the sweep would run
     * off a file Atlas has declared unreadable.
     *
     * This is not `semantic_auto_default`'s situation and its justification does
     * not transfer. That accessor is insensitive to `state` because gating it
     * changes no intended answer -- it fails *toward* its own default. This one
     * would fail *away* from a default the season deliberately set to false. The
     * precedent that fits is P0's `atlas_syspolicy_watch_max_dirs_total_checked`.
     */
    atlas_syspolicy p;
    memset(&p, 0, sizeof p);
    p.state = ATLAS_SYSPOLICY_LEGACY;
    p.reason = ATLAS_SYSPOLICY_REASON_MALFORMED;
    p.memory_reconcile = 1;
    p.memory_source_count = 2;
    p.memory_sources[0].cls = ATLAS_MEMORY_SOURCE_REPO_FILE;
    (void)snprintf(p.memory_sources[0].path, sizeof p.memory_sources[0].path, "CLAUDE.md");
    p.memory_sources[1].cls = ATLAS_MEMORY_SOURCE_EXTERNAL_DIR;
    (void)snprintf(p.memory_sources[1].path, sizeof p.memory_sources[1].path, "/srv/mem");

    T_CHECK_MSG(atlas_syspolicy_memory_reconcile_effective_checked(&p) ==
                    ATLAS_MEMORY_RECONCILE_DEFAULT,
                "a policy that did not parse turned the sweep on");
    T_CHECK_MSG(atlas_syspolicy_memory_source_count_checked(&p) == 0,
                "a policy that did not parse registered a memory source");
    T_CHECK_MSG(atlas_syspolicy_memory_source_at_checked(&p, 0) == NULL,
                "a policy that did not parse handed out a memory source");

    /* The unchecked flag accessor keeps the brief's semantics and is what shows
     * the difference: it still answers from the field alone. */
    T_CHECK(atlas_syspolicy_memory_reconcile_effective(&p));

    /* The same struct, once it is a policy that parsed. Nothing about the fields
     * changed; what changed is that Atlas can now read the file back. */
    p.state = ATLAS_SYSPOLICY_SYSTEM;
    p.reason = ATLAS_SYSPOLICY_REASON_ACTIVE;
    T_CHECK(atlas_syspolicy_memory_reconcile_effective_checked(&p));
    T_CHECK(atlas_syspolicy_memory_source_count_checked(&p) == 2);
    const struct atlas_syspolicy_memory_source *s = atlas_syspolicy_memory_source_at_checked(&p, 0);
    T_REQUIRE(s != NULL);
    T_EQ_INT((int)s->cls, (int)ATLAS_MEMORY_SOURCE_REPO_FILE);
    T_EQ_STR(s->path, "CLAUDE.md");
    s = atlas_syspolicy_memory_source_at_checked(&p, 1);
    T_REQUIRE(s != NULL);
    T_EQ_STR(s->path, "/srv/mem");
    /* Past the count is refused rather than read out of the tail of the array,
     * which is still full of whatever the parser left there. */
    T_CHECK(atlas_syspolicy_memory_source_at_checked(&p, 2) == NULL);
    T_CHECK(atlas_syspolicy_memory_source_at_checked(&p, ATLAS_MEMORY_MAX_SOURCES) == NULL);

    /* DISABLED on a policy that parsed is still DISABLED. */
    p.memory_reconcile = 2;
    T_CHECK(!atlas_syspolicy_memory_reconcile_effective_checked(&p));

    /* No policy at all is the ordinary per-user install: no statement, so the
     * named default, and nothing registered. */
    T_CHECK(atlas_syspolicy_memory_reconcile_effective_checked(NULL) ==
            ATLAS_MEMORY_RECONCILE_DEFAULT);
    T_CHECK(atlas_syspolicy_memory_source_count_checked(NULL) == 0);
    T_CHECK(atlas_syspolicy_memory_source_at_checked(NULL, 0) == NULL);
}

/* --- through the loader ----------------------------------------------------
 *
 * What this half can and cannot establish, stated rather than implied.
 *
 * `atlas_syspolicy_load_at` reaches the file through `atlas_rootpath_open`,
 * which walks from `/` and requires **every** component to be owned by uid 0
 * and writable by nobody else (`src/core/rootpath.c:43`). A fixture lives under
 * `TMPDIR`, which is `/tmp` at mode 1777 — other-writable — so the walk refuses
 * at the first component and the file is never opened. The `key = value` loop is
 * therefore unreachable from a test process that is not root, and no body below
 * can be observed to produce `ATLAS_SYSPOLICY_REASON_MALFORMED`: the reason is
 * WRITABLE, decided before a byte is read.
 *
 * So this asserts what is provable without privilege, which is the property
 * that matters: **no shape an unprivileged uid can construct anywhere on the
 * filesystem registers a memory source or turns the sweep on.** That is
 * `tests/test_watch_budget.c`'s answer to the identical wall for P0's
 * `watch_max_dirs_total`, and `tests/test_a71_syspolicy.c`'s header states the
 * split. The grammar itself is proved above, against
 * `atlas_memory_source_value_parse` — which is exactly why that function is
 * exposed rather than being a static inside the loader. */

static void load_body(const fixture *fx, const char *name, const char *body, atlas_syspolicy *out,
                      atlas_err *err) {
    T_OK(fx_write(fx_data_dir(fx), name, body, err), err);
    char path[1024];
    (void)snprintf(path, sizeof(path), "%s/%s", fx_data_dir(fx), name);
    atlas_syspolicy_load_at(path, out);
}

static void test_no_unprivileged_policy_registers_a_memory_source(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_OK(fx_open(&fx, &err), &err);

    /* One more `memory_source` line than the bound admits. Refused rather than
     * truncated — `client_uid`'s rule — because a silently shortened list is one
     * whose author and reader disagree about what is on it. */
    char overflow[8192];
    size_t used = (size_t)snprintf(overflow, sizeof overflow,
                                   "socket_path = /run/atlas/atlas.sock\n"
                                   "data_dir = /var/lib/atlas\n");
    for (unsigned i = 0; i < ATLAS_MEMORY_MAX_SOURCES + 1u; i++) {
        used += (size_t)snprintf(overflow + used, sizeof overflow - used,
                                 "memory_source = REPO_FILE:m%u.md\n", i);
        T_REQUIRE(used < sizeof overflow);
    }

#define HEAD "socket_path = /run/atlas/atlas.sock\ndata_dir = /var/lib/atlas\n"
    const char *const bodies[] = {
        overflow,
        /* An unrecognised sweep spelling; the two recognised ones; the key
         * stated twice; neither key at all; a well-formed source -- the file an
         * unprivileged attacker would write; and a source the grammar refuses. */
        HEAD "memory_reconcile = yes\n",
        HEAD "memory_reconcile = ENABLED\n",
        HEAD "memory_reconcile = DISABLED\n",
        HEAD "memory_reconcile = ENABLED\nmemory_reconcile = DISABLED\n",
        HEAD,
        HEAD "memory_source = REPO_FILE:CLAUDE.md\n",
        HEAD "memory_source = REPO_FILE:../escape\n",
    };
#undef HEAD

    for (size_t i = 0; i < sizeof bodies / sizeof bodies[0]; i++) {
        char name[64];
        (void)snprintf(name, sizeof name, "memory-%zu.conf", i);
        atlas_syspolicy p;
        load_body(&fx, name, bodies[i], &p, &err);
        T_CHECK_MSG(p.state != ATLAS_SYSPOLICY_SYSTEM,
                    "a policy written by this uid reached system mode (case %zu)", i);
        T_CHECK_MSG(p.memory_source_count == 0,
                    "a policy written by this uid registered a memory source (case %zu)", i);
        T_CHECK_MSG(p.memory_reconcile == 0,
                    "a policy written by this uid stated a sweep intent (case %zu)", i);
        T_CHECK_MSG(!atlas_syspolicy_memory_reconcile_effective(&p),
                    "a policy written by this uid turned the sweep on (case %zu)", i);
    }

    /* Case 5 has neither key and is otherwise a complete policy: it must not be
     * the *grammar* that refused the ones above. Recorded rather than asserted
     * either way, for `test_a71_syspolicy.c`'s reason — on this machine it fails
     * the ownership walk like every other fixture file. */
    atlas_test_note("the loader ran as %s, so the parser loop was %s",
                    geteuid() == 0 ? "root" : "an unprivileged uid",
                    geteuid() == 0 ? "reachable" : "never entered");

    fx_close(&fx);
}

static const atlas_test TESTS[] = {
    {"UNKNOWN is zero in every vocabulary", test_unknown_is_zero_in_every_vocabulary},
    {"the zero member is named UNKNOWN and never parses",
     test_the_zero_member_is_named_unknown_and_never_parses},
    {"every source class round trips", test_every_source_class_round_trips},
    {"every diff kind round trips", test_every_diff_kind_round_trips},
    {"every anchor kind round trips", test_every_anchor_kind_round_trips},
    {"every pack status round trips", test_every_pack_status_round_trips},
    {"every generation cause round trips", test_every_gen_cause_round_trips},
    {"is_repo is true for exactly the two repository classes",
     test_is_repo_is_true_for_exactly_the_two_repo_classes},
    {"the value grammar accepts exactly what it should",
     test_the_value_grammar_accepts_exactly_what_it_should},
    {"the value grammar refuses rather than repairs",
     test_the_value_grammar_refuses_rather_than_repairs},
    {"the sweep gate resolves UNSET to the named default",
     test_the_sweep_gate_resolves_unset_to_the_named_default},
    {"a policy Atlas could not read registers nothing",
     test_a_policy_atlas_could_not_read_registers_nothing},
    {"no unprivileged policy registers a memory source",
     test_no_unprivileged_policy_registers_a_memory_source},
};

ATLAS_TEST_MAIN("memory_source", TESTS)

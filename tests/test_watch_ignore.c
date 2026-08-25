/* Atlas - P0: what the watcher does about directories git ignores.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Two defects, and the second is the one that survived the first review.
 *
 * **The set did not exist at event time.** The ignored-path inventory was built
 * on the stack inside the priming function and freed when it returned, and the
 * handler for "a directory appeared" built its context with `memset`, leaving
 * the inventory pointer NULL. So a directory created while the daemon ran was
 * watched recursively whatever `.gitignore` said — every rebuilt `build/`, every
 * restored `node_modules`, for as long as the daemon lived, and never reclaimed
 * until a repository was added or removed.
 *
 * **And carrying the set forward would not have been enough.** `git ls-files
 * --others --ignored --directory` enumerates the *filesystem*. A `.gitignore`
 * containing `build/` with no `build/` on disk produces no entry at all, so an
 * inventory built at priming has nothing to say about a directory created
 * afterwards — which is precisely the directory being asked about. Consulting it
 * would have answered "not ignored" with confidence, for exactly the case the
 * whole mechanism exists to handle.
 *
 * So the inventory is only ever an inventory of paths that existed when it was
 * read, and it is never the authority on a path that did not. A directory Atlas
 * has not seen before waits in a bounded queue while one `git ls-files` per
 * debounce tick answers for the whole queue at once — and because the directory
 * now exists on disk, that answer is correct.
 *
 * Waiting has a cost, and it is asserted here rather than hidden: nothing under
 * the directory is watched until the answer arrives, so events inside it in the
 * meantime are missed. That is an event gap, and the repository may not be
 * described as current until a content-verifying pass has run.
 */
#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas_test.h"
#include "support/fixture.h"

#define WAIT_MS 30000

typedef struct live {
    fixture fx;
    fx_daemon d;
} live;

/* A repository whose `.gitignore` names `build/` from its first commit, with
 * **no `build/` directory on disk**, registered and watched.
 *
 * The absence is the fixture. It is what makes the inventory empty at priming,
 * and therefore what makes a cached answer wrong later. */
static void live_start(live *L, atlas_err *err) {
    T_OK(fx_open(&L->fx, err), err);
    T_OK(fx_init_repo(&L->fx, fx_repo(&L->fx), NULL, err), err);
    T_OK(fx_write(fx_repo(&L->fx), ".gitignore", "build/\n", err), err);
    T_OK(fx_write(fx_repo(&L->fx), "a.c", "int a;\n", err), err);
    T_OK(fx_mkdir(fx_repo(&L->fx), "src", err), err);
    T_OK(fx_write(fx_repo(&L->fx), "src/b.c", "int b;\n", err), err);
    T_OK(fx_add_all(&L->fx, fx_repo(&L->fx), err), err);
    T_OK(fx_commit(&L->fx, fx_repo(&L->fx), "first", err), err);

    const char *add[] = {"--data-dir", fx_data_dir(&L->fx), "repo", "add", fx_repo(&L->fx),
                         "--name", "fixture"};
    int code = 0;
    T_OK(fx_atlas(add, 7u, NULL, NULL, &code, err), err);
    T_EQ_INT(code, 0);

    fx_daemon_init(&L->d);
    T_OK(fx_daemon_start(&L->fx, &L->d, err), err);
    T_OK(fx_daemon_wait_ready(&L->d, WAIT_MS, err), err);
}

static void live_stop(live *L) {
    fx_daemon_stop(&L->d, false);
    fx_daemon_free(&L->d);
    fx_close(&L->fx);
}

static void cli(live *L, const char *const *args, size_t n, atlas_buf *out, int *code,
                atlas_err *err) {
    T_OK(fx_atlas_with_runtime(&L->fx, &L->d, args, n, out, NULL, code, err), err);
}

static bool wait_events(live *L, const char *needle, atlas_err *err) {
    const char *args[] = {"events", "fixture", "--json", "--limit", "500"};
    bool found = false;
    T_OK(fx_wait_for_substring(&L->fx, &L->d, args, 5u, needle, WAIT_MS, &found, err), err);
    return found;
}

/* The repository's state document, as the daemon reports it. */
static void repo_state(live *L, atlas_buf *out, atlas_err *err) {
    const char *args[] = {"events", "fixture", "--json", "--limit", "1"};
    int code = -1;
    atlas_buf_reset(out);
    cli(L, args, 5u, out, &code, err);
}

/* Reads an integer field out of the state document. -1 when absent. */
static long state_int(const atlas_buf *doc, const char *key) {
    char needle[128];
    (void)snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(atlas_buf_cstr(doc), needle);
    if (p == NULL) {
        return -1;
    }
    return strtol(p + strlen(needle), NULL, 10);
}

static bool state_says(const atlas_buf *doc, const char *fragment) {
    return strstr(atlas_buf_cstr(doc), fragment) != NULL;
}

/* Waits until the repository reports a complete, current index.
 *
 * This is the condition the whole design has to reach: watches installed,
 * nothing pending, the event gap resolved by a pass that actually read the
 * bytes. It is polled rather than slept for, because how many ticks it takes is
 * a property of the machine. */
static bool wait_current(live *L, atlas_buf *doc, atlas_err *err) {
    for (int i = 0; i < WAIT_MS / 100; i++) {
        repo_state(L, doc, err);
        if (state_says(doc, "\"index_current\":true") &&
            state_says(doc, "\"event_gap\":false") &&
            state_says(doc, "\"pending_full_reconcile\":false") &&
            state_says(doc, "\"watch_state\":\"watching\"")) {
            return true;
        }
        usleep(100000);
    }
    return false;
}

/* --- the mandatory regression: a future-created ignored directory ----------- */

/* `.gitignore` names `build/` from the first commit; `build/` does not exist
 * when the daemon primes; `build/a/b/c` is created afterwards.
 *
 * Nothing under `build` may be watched at any depth, the repository must not
 * degrade, and a visible sibling created at the same moment must be indexed —
 * that last part is what proves the pass ran at all rather than that nothing
 * happened.
 *
 * Run in three shapes, because the *first* event the watcher sees differs
 * between them and the empty-directory shape is the one that arrives first in
 * real life: `mkdir build` produces `IN_CREATE` before any file exists inside
 * it, and an inventory consulted at that instant must still answer correctly.
 */
static void run_future_ignored_case(const char *label, bool deep, bool with_files) {
    live L;
    atlas_err err;
    atlas_err_init(&err);
    live_start(&L, &err);
    T_CHECK(wait_events(&L, "\"kind\":\"reconciled\"", &err));

    atlas_buf doc = ATLAS_BUF_INIT;
    T_CHECK_MSG(wait_current(&L, &doc, &err), "%s: the repository never became current to start",
                label);
    long watched_before = state_int(&doc, "watched_source");

    /* The ignored tree, created now. */
    T_OK(fx_mkdir(fx_repo(&L.fx), "build", &err), &err);
    if (deep) {
        T_OK(fx_mkdir(fx_repo(&L.fx), "build/a", &err), &err);
        T_OK(fx_mkdir(fx_repo(&L.fx), "build/a/b", &err), &err);
        T_OK(fx_mkdir(fx_repo(&L.fx), "build/a/b/c", &err), &err);
    }
    if (with_files) {
        T_OK(fx_write(fx_repo(&L.fx), deep ? "build/a/b/c/f.o" : "build/f.o", "junk\n", &err),
             &err);
    }
    /* A visible sibling at the same moment, so "nothing was indexed" cannot pass
     * for "the ignored tree was skipped". */
    T_OK(fx_mkdir(fx_repo(&L.fx), "src/visible", &err), &err);
    T_OK(fx_write(fx_repo(&L.fx), "src/visible/g.c", "int g;\n", &err), &err);

    T_CHECK_MSG(wait_events(&L, "src/visible/g.c", &err),
                "%s: the visible sibling must be indexed", label);

    /* And the repository comes back to current: the visible subtree was watched
     * late, which is an event gap, and the gap must be resolved rather than
     * left standing. */
    T_CHECK_MSG(wait_current(&L, &doc, &err),
                "%s: the repository must return to current after the decision", label);

    /* Nothing under build/ was ever indexed. */
    atlas_buf events = ATLAS_BUF_INIT;
    int code = -1;
    const char *ev[] = {"events", "fixture", "--json", "--limit", "500"};
    cli(&L, ev, 5u, &events, &code, &err);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&events), "build/") == NULL,
                "%s: an ignored file must never be indexed", label);

    /* And no watch was spent on it. The visible sibling accounts for exactly one
     * new source directory; an ignored tree watched in full would show four more
     * in the deep shape. Asserted as an exact delta rather than a bound, because
     * a bound would pass against the defect. */
    long watched_after = state_int(&doc, "watched_source");
    T_CHECK_MSG(watched_after == watched_before + 1,
                "%s: expected exactly one new watch for the visible sibling, went from %ld to %ld",
                label, watched_before, watched_after);
    T_CHECK_MSG(state_says(&doc, "\"watch_state\":\"watching\""),
                "%s: a repository that correctly skipped an ignored tree is not degraded", label);
    T_CHECK_MSG(state_says(&doc, "\"watch_reason\":\"none\""),
                "%s: a complete watch set carries no degradation reason", label);

    atlas_buf_free(&events);
    atlas_buf_free(&doc);
    live_stop(&L);
}

static void test_future_ignored_directory_empty(void) {
    /* `mkdir build` and nothing else — the first event the watcher ever sees for
     * this path, and the one an inventory is least able to answer. */
    run_future_ignored_case("empty build/", false, false);
}

static void test_future_ignored_directory_deep_empty(void) {
    run_future_ignored_case("deep empty build/a/b/c", true, false);
}

static void test_future_ignored_directory_deep_with_files(void) {
    run_future_ignored_case("deep build/a/b/c with files", true, true);
}

/* --- the binding clarification: the decision window is an event gap --------- */

/* A *visible* directory is created and written into immediately, before the
 * ignore decision for it can have completed.
 *
 * While it waits, nothing inside it is watched, so those writes produce no
 * events. Atlas must still end up seeing every eligible file, must not claim to
 * be current before a content-verifying pass has run, and must finish with the
 * gap closed.
 *
 * This is the case that makes the queue honest rather than merely bounded: the
 * cost of waiting is real, and the answer is to record it as a gap rather than
 * to pretend the wait was free. */
static void test_writes_during_the_ignore_decision_are_not_lost(void) {
    live L;
    atlas_err err;
    atlas_err_init(&err);
    live_start(&L, &err);
    T_CHECK(wait_events(&L, "\"kind\":\"reconciled\"", &err));

    atlas_buf doc = ATLAS_BUF_INIT;
    T_CHECK(wait_current(&L, &doc, &err));

    /* Create and immediately fill, with no wait in between: the nested
     * directories and their files land inside the decision window. */
    T_OK(fx_mkdir(fx_repo(&L.fx), "late", &err), &err);
    T_OK(fx_mkdir(fx_repo(&L.fx), "late/inner", &err), &err);
    T_OK(fx_mkdir(fx_repo(&L.fx), "late/inner/deeper", &err), &err);
    T_OK(fx_write(fx_repo(&L.fx), "late/one.c", "int one;\n", &err), &err);
    T_OK(fx_write(fx_repo(&L.fx), "late/inner/two.c", "int two;\n", &err), &err);
    T_OK(fx_write(fx_repo(&L.fx), "late/inner/deeper/three.c", "int three;\n", &err), &err);

    /* Every eligible file is eventually seen — including the ones written while
     * nothing was watching, which only a full content-verifying pass can find. */
    T_CHECK_MSG(wait_events(&L, "late/one.c", &err), "a file written during the decision window "
                                                     "must still be indexed");
    T_CHECK_MSG(wait_events(&L, "late/inner/two.c", &err), "a nested file written during the "
                                                           "decision window must still be indexed");
    T_CHECK_MSG(wait_events(&L, "late/inner/deeper/three.c", &err),
                "a deeply nested file written during the decision window must still be indexed");

    /* And it ends current, with the gap closed and the pass no longer owed. */
    T_CHECK_MSG(wait_current(&L, &doc, &err),
                "the repository must reach event_gap=false, pending_full_reconcile=false and "
                "index_current=true");

    atlas_buf_free(&doc);
    live_stop(&L);
}

/* --- ignore rules that change while the daemon runs ------------------------ */

/* A visible subtree becomes ignored: its watches are released and it stops being
 * indexed. And then the reverse: an ignored subtree becomes visible and is both
 * watched and indexed.
 *
 * Neither is triggered by a repository-set change, which is the point — before
 * P0 the inventory was rebuilt only when a repository was added or removed, so a
 * `.gitignore` edit changed what was indexed and left the watch set describing
 * the rules as they had been at startup. */
static void test_gitignore_edit_takes_effect_without_a_repo_set_change(void) {
    live L;
    atlas_err err;
    atlas_err_init(&err);
    live_start(&L, &err);
    T_CHECK(wait_events(&L, "\"kind\":\"reconciled\"", &err));

    atlas_buf doc = ATLAS_BUF_INIT;
    T_CHECK(wait_current(&L, &doc, &err));

    /* A visible tree, watched and indexed. */
    T_OK(fx_mkdir(fx_repo(&L.fx), "generated", &err), &err);
    T_OK(fx_write(fx_repo(&L.fx), "generated/x.c", "int x;\n", &err), &err);
    T_CHECK(wait_events(&L, "generated/x.c", &err));
    T_CHECK(wait_current(&L, &doc, &err));
    long with_generated = state_int(&doc, "watched_source");

    /* Now ignore it, by editing `.gitignore` and nothing else. */
    T_OK(fx_write(fx_repo(&L.fx), ".gitignore", "build/\ngenerated/\n", &err), &err);

    /* The watch on the newly ignored tree is released. Polled, because the
     * re-prime happens on the watcher's own tick. */
    bool released = false;
    for (int i = 0; i < WAIT_MS / 100; i++) {
        repo_state(&L, &doc, &err);
        long now = state_int(&doc, "watched_source");
        if (now >= 0 && now < with_generated) {
            released = true;
            break;
        }
        usleep(100000);
    }
    T_CHECK_MSG(released,
                "editing .gitignore must release the watches on a newly ignored subtree without a "
                "repository-set change");

    /* And the repository comes back to current: the re-prime is a window in
     * which events were not observed, so it owes a full pass and then closes it. */
    T_CHECK_MSG(wait_current(&L, &doc, &err),
                "the repository must return to current after an ignore-rule change");

    /* The reverse: make it visible again and it is watched and indexed again. */
    T_OK(fx_write(fx_repo(&L.fx), ".gitignore", "build/\n", &err), &err);
    T_OK(fx_write(fx_repo(&L.fx), "generated/y.c", "int y;\n", &err), &err);
    T_CHECK_MSG(wait_events(&L, "generated/y.c", &err),
                "a subtree that stops being ignored must be watched and indexed again");
    T_CHECK_MSG(wait_current(&L, &doc, &err),
                "the repository must return to current after the rules change back");

    atlas_buf_free(&doc);
    live_stop(&L);
}

/* `.git/info/exclude` is a git ignore source that lives outside the working
 * tree, and an inotify watch on the git directory does **not** report changes to
 * it: a directory watch reports its direct children only, and `exclude` is one
 * level down. Verified by experiment before this test was written — watching
 * `.git` and modifying `.git/info/exclude` produced no events at all, while
 * watching `.git/info` produced two.
 *
 * So `info/` is subscribed explicitly. This asserts the consequence: a rule
 * written there takes effect without any working-tree change to trigger it. */
static void test_info_exclude_change_takes_effect(void) {
    live L;
    atlas_err err;
    atlas_err_init(&err);
    live_start(&L, &err);
    T_CHECK(wait_events(&L, "\"kind\":\"reconciled\"", &err));

    atlas_buf doc = ATLAS_BUF_INIT;
    T_CHECK(wait_current(&L, &doc, &err));

    T_OK(fx_mkdir(fx_repo(&L.fx), "scratch", &err), &err);
    T_OK(fx_write(fx_repo(&L.fx), "scratch/z.c", "int z;\n", &err), &err);
    T_CHECK(wait_events(&L, "scratch/z.c", &err));
    T_CHECK(wait_current(&L, &doc, &err));
    long before = state_int(&doc, "watched_source");

    /* Ignore it through `info/exclude`, touching nothing in the working tree. */
    atlas_buf excl = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&excl, &err, "%s/.git/info", fx_repo(&L.fx)), &err);
    T_OK(fx_mkdir(fx_repo(&L.fx), ".git/info", &err), &err);
    T_OK(fx_write(fx_repo(&L.fx), ".git/info/exclude", "scratch/\n", &err), &err);

    bool released = false;
    for (int i = 0; i < WAIT_MS / 100; i++) {
        repo_state(&L, &doc, &err);
        long now = state_int(&doc, "watched_source");
        if (now >= 0 && now < before) {
            released = true;
            break;
        }
        usleep(100000);
    }
    T_CHECK_MSG(released,
                "a rule written to .git/info/exclude must reach the watcher; if this fails the "
                "info/ subscription is missing and only the periodic pass would ever notice");

    atlas_buf_free(&excl);
    atlas_buf_free(&doc);
    live_stop(&L);
}

/* --- a burst costs one git invocation per tick ----------------------------- */

/* Many directories appearing at once must not become many git processes.
 *
 * The queue exists for this: an unpacked archive or a build that creates a
 * thousand directories in a burst would otherwise mean a thousand
 * `git check-ignore` invocations, which is why the decision is batched and
 * debounced rather than asked per directory. What is asserted is the outcome —
 * every visible directory ends up watched and the repository ends up current —
 * within a deadline a per-directory implementation could not meet. */
static void test_a_burst_of_new_directories_resolves(void) {
    live L;
    atlas_err err;
    atlas_err_init(&err);
    live_start(&L, &err);
    T_CHECK(wait_events(&L, "\"kind\":\"reconciled\"", &err));

    atlas_buf doc = ATLAS_BUF_INIT;
    T_CHECK(wait_current(&L, &doc, &err));
    long before = state_int(&doc, "watched_source");

    const int N = 200;
    for (int i = 0; i < N; i++) {
        char d[64];
        (void)snprintf(d, sizeof(d), "burst%03d", i);
        T_OK(fx_mkdir(fx_repo(&L.fx), d, &err), &err);
    }
    /* One file, so there is something to wait for. */
    T_OK(fx_write(fx_repo(&L.fx), "burst199/last.c", "int l;\n", &err), &err);

    T_CHECK(wait_events(&L, "burst199/last.c", &err));
    T_CHECK_MSG(wait_current(&L, &doc, &err), "a burst of new directories must resolve and the "
                                              "repository must return to current");
    long after = state_int(&doc, "watched_source");
    T_CHECK_MSG(after == before + N,
                "expected %d new watches for %d new visible directories, went from %ld to %ld", N,
                N, before, after);

    atlas_buf_free(&doc);
    live_stop(&L);
}

/* The intermediate state is observed, not inferred from the end state.
 *
 * A large visible subtree appears at once. Two things must be true *while* it is
 * being dealt with, and neither is implied by eventual convergence:
 *
 *   1. The repository is persisted as `priming` and NOT current **before** the
 *      ignore decision or the walk begins. A directory that has just appeared is
 *      already an unobserved subtree; a reader arriving in that window must not
 *      be told `watching`.
 *   2. The walk is chunked. A 65 000-entry tree walked synchronously inside one
 *      watcher tick would drain no inotify events for its duration, which is how
 *      a queue overflow is manufactured — and an overflow gaps every repository
 *      at once. The test asserts the daemon keeps answering throughout and that
 *      the watch count climbs rather than jumping in one step.
 *
 * This is the case that distinguishes "it got there in the end" from "it never
 * claimed something untrue on the way". */
static void test_a_late_subtree_is_reported_priming_and_walked_in_chunks(void) {
    live L;
    atlas_err err;
    atlas_err_init(&err);
    live_start(&L, &err);
    T_CHECK(wait_events(&L, "\"kind\":\"reconciled\"", &err));

    atlas_buf doc = ATLAS_BUF_INIT;
    T_CHECK(wait_current(&L, &doc, &err));
    long before = state_int(&doc, "watched_source");

    /* A visible tree large enough that a synchronous walk would be visible as a
     * stall, created as one burst. */
    /* Comfortably more than ATLAS_WATCH_PRIME_CHUNK_DIRS (512), so the walk
     * must span several ticks and an intermediate count is observable — and
     * small enough that the full content-verifying pass it owes afterwards
     * finishes inside the deadline. */
    const int N = 1500;
    T_OK(fx_mkdir(fx_repo(&L.fx), "bulk", &err), &err);
    for (int i = 0; i < N; i++) {
        char d[64];
        (void)snprintf(d, sizeof(d), "bulk/d%04d", i);
        T_OK(fx_mkdir(fx_repo(&L.fx), d, &err), &err);
    }

    /* Sample the *persisted* state repeatedly while it settles. */
    bool saw_priming_not_current = false;
    bool saw_intermediate_count = false;
    long last = before;
    for (int i = 0; i < WAIT_MS / 50; i++) {
        repo_state(&L, &doc, &err);
        bool priming = state_says(&doc, "\"watch_state\":\"priming\"");
        bool current = state_says(&doc, "\"index_current\":true");
        if (priming) {
            T_CHECK_MSG(!current,
                        "a repository reported `priming` must never also be reported current");
            saw_priming_not_current = true;
        }
        long now = state_int(&doc, "watched_source");
        if (now > last && now < before + N) {
            /* A count strictly between the start and the end is proof the walk
             * was published in pieces rather than in one step. */
            saw_intermediate_count = true;
        }
        if (now > last) {
            last = now;
        }
        if (state_says(&doc, "\"watch_state\":\"watching\"") && now >= before + N) {
            break;
        }
        usleep(50000);
    }

    T_CHECK_MSG(saw_priming_not_current,
                "the repository must be persisted as priming and not current while a newly "
                "appeared subtree is being resolved and walked");
    T_CHECK_MSG(saw_intermediate_count,
                "the walk must be chunked: an intermediate watch count between %ld and %ld was "
                "never observed, which is what a single synchronous walk looks like",
                before, before + N);

    /* And it converges: everything watched, gap closed, current again. */
    T_CHECK_MSG(wait_current(&L, &doc, &err),
                "the repository must return to current once the subtree is watched");
    long after = state_int(&doc, "watched_source");
    T_CHECK_MSG(after == before + N + 1,
                "expected %ld watches for %d new directories plus their parent, got %ld",
                before + N + 1, N, after);

    atlas_buf_free(&doc);
    live_stop(&L);
}


/* P0. A directory that becomes ignored **while a late subtree is being walked**
 * keeps no watch.
 *
 * The post-walk sweep exists because a walk is not instantaneous. It is judged
 * against an ignore inventory read before it started, and spends several ticks
 * installing watches; a directory created during those ticks is being judged
 * against an inventory that predates it. For the *initial* prime that was
 * already covered. A late subtree is pushed onto the frontier by the event
 * handler — a different entry point — and it did not ask for the sweep, so an
 * ignored directory that appeared mid-walk kept the watches the walk gave it
 * until the next repository-set change. On a repository whose build output is
 * ignored, that is the entire defect P0 exists to fix, arrived at through a door
 * nobody had closed.
 *
 * Reaching it takes some care, because two other mechanisms cover most of the
 * ground and would make a careless test unable to fail:
 *
 *   - a directory whose parent is already watched produces an `IN_CREATE`, goes
 *     into the pending-ignore queue and is judged against a *fresh* inventory;
 *   - and that queue's refresh re-reads the inventory for the rest of the walk,
 *     so a single event anywhere covers every directory the walk has not reached
 *     yet.
 *
 * So the subtree here is one deep chain and the ignored directory is created at
 * the very bottom of it, below the depth the first chunk of the walk can reach.
 * Nothing is watching its parent, so no event is produced, nothing is queued,
 * no refresh happens, and the walk installs a watch on a directory git ignores.
 * Only a sweep taken after the walk finishes can release it. */
static void test_a_directory_ignored_during_a_late_walk_keeps_no_watch(void) {
    live L;
    atlas_err err;
    atlas_err_init(&err);
    live_start(&L, &err);
    T_CHECK(wait_events(&L, "\"kind\":\"reconciled\"", &err));

    atlas_buf doc = ATLAS_BUF_INIT;
    T_CHECK(wait_current(&L, &doc, &err));
    long before = state_int(&doc, "watched_source");

    /* Deeper than ATLAS_WATCH_PRIME_CHUNK_DIRS (512), so the first chunk cannot
     * reach the bottom, and short enough that the whole path stays well inside
     * PATH_MAX. */
    const int DEPTH = 600;
    atlas_buf chain = ATLAS_BUF_INIT;
    T_OK(atlas_buf_set_str(&chain, "bulk", &err), &err);
    T_OK(fx_mkdir(fx_repo(&L.fx), "bulk", &err), &err);
    for (int i = 0; i < DEPTH; i++) {
        char seg[8];
        (void)snprintf(seg, sizeof(seg), "/c%03d", i);
        T_OK(atlas_buf_append_str(&chain, seg, &err), &err);
        T_OK(fx_mkdir(fx_repo(&L.fx), atlas_buf_cstr(&chain), &err), &err);
    }
    /* A visible file at the bottom, so that git does not report the bottom
     * directory itself as entirely ignored once `build` appears inside it —
     * which would be git's correct answer and would release a watch this test
     * expects to survive. */
    atlas_buf keep = ATLAS_BUF_INIT;
    T_OK(atlas_buf_set(&keep, chain.data, chain.len, &err), &err);
    T_OK(atlas_buf_append_str(&keep, "/keep.c", &err), &err);
    T_OK(fx_write(fx_repo(&L.fx), atlas_buf_cstr(&keep), "int k;\n", &err), &err);
    atlas_buf_free(&keep);

    /* The moment the walk publishes any progress it has started and cannot yet
     * have reached the bottom. Create the ignored directory there. */
    bool started = false;
    for (int i = 0; i < WAIT_MS / 10 && !started; i++) {
        repo_state(&L, &doc, &err);
        started = state_int(&doc, "watched_source") > before;
        if (!started) {
            usleep(10000);
        }
    }
    T_CHECK_MSG(started, "the late subtree's walk never started");
    atlas_buf deep = ATLAS_BUF_INIT;
    T_OK(atlas_buf_set(&deep, chain.data, chain.len, &err), &err);
    T_OK(atlas_buf_append_str(&deep, "/build", &err), &err);
    T_OK(fx_mkdir(fx_repo(&L.fx), atlas_buf_cstr(&deep), &err), &err);
    atlas_buf_free(&deep);
    atlas_buf_free(&chain);

    /* It settles: the chain is watched, the ignored directory at its bottom is
     * not, and the repository is current again. */
    T_CHECK_MSG(wait_current(&L, &doc, &err),
                "the repository must return to current after the late subtree is walked");
    long after = state_int(&doc, "watched_source");
    T_CHECK_MSG(after == before + DEPTH + 1,
                "expected %ld watches for a chain of %d plus its root and none for the ignored "
                "directory created during the walk, got %ld",
                before + DEPTH + 1, DEPTH, after);
    T_CHECK_MSG(!state_says(&doc, "\"watch_state\":\"degraded\""),
                "an ignored directory appearing mid-walk must not degrade the repository");

    atlas_buf_free(&doc);
    live_stop(&L);
}

static const atlas_test TESTS[] = {
    {"an ignored directory created empty after priming consumes no watch",
     test_future_ignored_directory_empty},
    {"a deep empty ignored tree created after priming consumes no watch",
     test_future_ignored_directory_deep_empty},
    {"a deep ignored tree with files created after priming consumes no watch",
     test_future_ignored_directory_deep_with_files},
    {"writes during the ignore decision are found and the gap is closed",
     test_writes_during_the_ignore_decision_are_not_lost},
    {"a .gitignore edit takes effect without a repository-set change",
     test_gitignore_edit_takes_effect_without_a_repo_set_change},
    {"a .git/info/exclude change reaches the watcher", test_info_exclude_change_takes_effect},
    {"a late subtree is reported priming and not current, and walked in chunks",
     test_a_late_subtree_is_reported_priming_and_walked_in_chunks},
    {"a burst of new directories resolves and every one is watched",
     test_a_burst_of_new_directories_resolves},
    {"a directory ignored during a late walk keeps no watch",
     test_a_directory_ignored_during_a_late_walk_keeps_no_watch},
};

ATLAS_TEST_MAIN("watch_ignore", TESTS)

/* Atlas - A9.2: the deterministic verifiers. A9.2.2: and their coverage.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A deterministic verifier evaluates one claim's stated truth condition
 * mechanically and reports PASS, FAIL or UNAVAILABLE. It is the only path in
 * A9.2 that can establish a claim without weighing anybody's reliability, which
 * is why every restriction on it matters more than it looks.
 *
 * ## What "deterministic" is allowed to mean
 *
 * Within the claim's declared scope, Atlas has a pass/fail procedure another run
 * over the same artifacts reproduces. **That is all.** It does not mean the
 * proposition is proven for every implementation for all time, and the gap
 * between those two readings is where semantic inflation lives: a unit test
 * establishing `f rejects y at commit z` establishes exactly that, and a claim
 * whose text says "the parser is safe" is not the claim the verifier checked.
 *
 * So every verifier here writes the scope it actually established into the
 * evidence it produces, and the claim's own `scope_note` is compared against
 * it. A claim that outruns its verifier is not verified; it is a claim with an
 * unverified remainder, and Atlas says so.
 *
 * ## Every verifier is a read
 *
 * None of them creates a process, runs a repository's build, executes a command
 * or opens a file the repository controls. That is a deliberate V1 restriction,
 * not an unfinished one. A verifier that ran a command named in configuration
 * would be a code-execution path with an audit trail attached, and the argument
 * for adding one belongs to whoever needs it — in writing, with the sandbox
 * already built. A8-CI's bounded-child pattern (`atlas_proc_run`, an explicit
 * argv vector, an empty environment, `RLIMIT_AS`, a wall clock and an idle
 * bound) is what that would have to reuse, and `docs/verification.md` says so.
 *
 * `atlas.no_proven_caller`, added by A9.2.2, holds to this: it is two bounded
 * `SELECT COUNT(*)` over tables Atlas already built, and it opens nothing.
 *
 * The practical consequence is worth stating plainly: Atlas can mechanically
 * establish facts about *what is recorded in its own index* — bytes at a
 * commit, symbols, proven edges and address-takes in a semantic generation —
 * and cannot mechanically establish facts that require running the software.
 * The second kind is empirical here, and empirical is in shadow.
 *
 * ## UNAVAILABLE is not FAIL — and A9.2.2 is where that stopped being partial
 *
 * The single most dangerous confusion available to this file. An index that has
 * not run cannot establish that a symbol is absent; reporting "could not look"
 * as "it is not there" is how a remediation detector closes an obligation that
 * is still outstanding.
 *
 * A9.2 stated that rule and implemented **half** of it. `atlas.symbol_absent`
 * refused to report PASS over a partial generation, correctly. But its mirror
 * went unguarded: `atlas.symbol_present` returned **FAIL** on `count == 0`
 * having checked only that *some* generation existed, and `atlas.proven_edge`
 * never consulted completeness at all — the flag was not even gathered. Over a
 * generation whose defining or calling translation unit failed to parse, both
 * reported "it is not there" for something they had not looked at. Downstream
 * that is worse than it sounds: `autolifecycle.c` maps FAIL to CONTRADICTED
 * with confidence 0, and the deterministic verdict overrides the attestation
 * fold entirely, so a partial index turned an unexamined claim into a
 * mechanically contradicted one at full confidence.
 *
 * The fix is structural rather than two extra `if`s. Every verifier now
 * computes a `atlas_verify_coverage_report` **before** it decides a check, and
 * every check goes through `settle()`, which applies the one asymmetry:
 *
 *   - a verdict meaning *the thing is there* is emitted whatever the coverage.
 *     An incomplete index cannot conjure a symbol that is not there, so finding
 *     one is finding one;
 *   - a verdict meaning *the thing is not there* is emitted only when every
 *     coverage dimension that verdict rests on is sufficient. Otherwise it is
 *     UNAVAILABLE — Atlas could not look, which is not a finding.
 *
 * Which of the two a given PASS is depends on the verifier, so `settle()` asks
 * `atlas_verify_verifier_truth_of_check` rather than assuming: PASS is the
 * *negative* conclusion for `atlas.symbol_absent` and `atlas.no_proven_caller`,
 * and the positive one for the rest.
 *
 * This is what keeps the two axes from contradicting each other. Without it a
 * single row could carry `state = CONTRADICTED` and `truth = UNKNOWN` — the
 * same mechanical evaluation disagreeing with itself across two fields, which
 * is a worse outcome than the defect it was meant to fix.
 */
#include "atlas/verify.h"

#include <stdio.h>
#include <string.h>

#include "atlas/atlas.h"

/* The verifier input is a bounded, structured argument Atlas parses, never a
 * command and never a pattern. Its grammar is `key=value` pairs separated by
 * semicolons, with no escaping and no quoting: a value containing a semicolon
 * is refused rather than interpreted, because an escape mechanism is the
 * beginning of a parser somebody can surprise.
 *
 * Values are bounded and are compared, never executed and never used to build a
 * path. */
static bool input_field(const char *input, const char *key, char *out, size_t out_size) {
    if (input == NULL || key == NULL || out == NULL || out_size == 0) {
        return false;
    }
    size_t klen = strlen(key);
    const char *p = input;
    while (*p != '\0') {
        const char *end = strchr(p, ';');
        size_t seg = end != NULL ? (size_t)(end - p) : strlen(p);
        if (seg > klen + 1u && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            size_t vlen = seg - klen - 1u;
            if (vlen >= out_size) {
                return false; /* refused, never truncated */
            }
            memcpy(out, p + klen + 1u, vlen);
            out[vlen] = '\0';
            return true;
        }
        if (end == NULL) {
            break;
        }
        p = end + 1;
    }
    return false;
}

/* What one verifier established, in Atlas' own words.
 *
 * Fixed sentences with the checked values substituted, so the evidence a
 * verifier produces names its own scope and cannot silently be read as wider
 * than it is. Nothing repository-controlled reaches the format string. */
static void scope_of(atlas_verify_verifier v, const char *a, const char *b, char *out,
                     size_t out_size) {
    switch (v) {
    case ATLAS_VERIFIER_CONTENT_HASH:
        (void)snprintf(out, out_size, "the recorded content of %s in the indexed worktree", a);
        break;
    case ATLAS_VERIFIER_SYMBOL_PRESENT:
        (void)snprintf(out, out_size, "the presence of a symbol named %s in the current complete "
                                      "semantic generation", a);
        break;
    case ATLAS_VERIFIER_SYMBOL_ABSENT:
        (void)snprintf(out, out_size, "the absence of any symbol named %s across the current "
                                      "complete semantic generation", a);
        break;
    case ATLAS_VERIFIER_PROVEN_EDGE:
        (void)snprintf(out, out_size, "a compiler-proved direct call from %s to %s in the current "
                                      "semantic generation", a, b != NULL ? b : "");
        break;
    case ATLAS_VERIFIER_NO_PROVEN_CALLER:
        /* The scope sentence carries the bound the verifier cannot check, and
         * carries it in Atlas' own words rather than in a comment nobody reads:
         * dynamic symbol lookup is invisible to the compiler, and code outside
         * the repository was never indexed. What *is* established is stated
         * exactly, and it is a smaller claim than "nothing calls it". */
        (void)snprintf(out, out_size,
                       "the absence of any caller of %s within the current complete semantic "
                       "generation, established from proved direct calls and from the symbol's "
                       "address never being taken; nothing is claimed about dynamic symbol "
                       "lookup or about code outside the indexed repository", a);
        break;
    case ATLAS_VERIFIER_NONE:
        (void)snprintf(out, out_size, "nothing");
        break;
    }
}

/* Fills the three semantic-index dimensions from one generation's state.
 *
 * Each of the three answers implies a different reason and a different remedy,
 * which is why they are not one boolean: nothing published, a look that missed
 * part of the tree, and a look at a tree the repository has since left. */
static void sem_coverage(atlas_verify_coverage_report *cov, bool indexed, bool complete,
                         bool current) {
    atlas_verify_coverage c;
    if (!indexed) {
        c = ATLAS_COVERAGE_UNKNOWN;
    } else if (!complete) {
        c = ATLAS_COVERAGE_PARTIAL;
    } else if (!current) {
        c = ATLAS_COVERAGE_STALE;
    } else {
        c = ATLAS_COVERAGE_COMPLETE;
    }
    cov->dims[ATLAS_COVDIM_SEMANTIC_GENERATION] = c;
    /* A complete generation is a parse of every translation unit the
     * compilation database named, which is what "every tracked source in scope
     * was read" means for a semantic question — and it covers generated sources
     * on the same footing, because a generated `.c` in the compilation database
     * is parsed exactly like any other. Where the generation is not complete,
     * neither claim can be made. */
    cov->dims[ATLAS_COVDIM_TRACKED_SOURCE] = c;
    cov->dims[ATLAS_COVDIM_GENERATED_SOURCE] = c;
    cov->dims[ATLAS_COVDIM_DIRECT_CALLS] = c;
}

/* §11. No A9.2.2 verifier observes a running system or reads deployed
 * configuration, so both dimensions are UNKNOWN for every one of them — and
 * that is the correct, fail-closed answer rather than a gap.
 *
 * The consequence is the one §11 asks for: a claim whose absence would depend
 * on what the deployment does can never be answered ABSENT from repository
 * bytes, because the dimensions it would rest on were never established.
 * Repository absence therefore cannot become operational absence by
 * construction, without a check anywhere deciding that it must not. */
static void unobservable_coverage(atlas_verify_coverage_report *cov) {
    cov->dims[ATLAS_COVDIM_RUNTIME_STATE] = ATLAS_COVERAGE_UNKNOWN;
    cov->dims[ATLAS_COVDIM_DEPLOYED_CONFIG] = ATLAS_COVERAGE_UNKNOWN;
}

/* The one place a check is emitted, and the one place §7's asymmetry lives.
 *
 * `raw_pass` is what the verifier's own truth condition evaluated to. Whether
 * that is the positive or the negative conclusion depends on the verifier, so
 * the polarity table is asked rather than assumed — `atlas.symbol_absent` and
 * `atlas.no_proven_caller` are the two whose PASS *is* the negative.
 *
 * A negative conclusion whose coverage cannot be shown sufficient becomes
 * UNAVAILABLE, which is "Atlas could not look" and is not evidence in either
 * direction. A positive one is emitted whatever the coverage. */
static void settle(atlas_verify_verifier v, bool raw_pass, atlas_verify_coverage_report *cov,
                   atlas_verify_check *check_out, char *detail_out, size_t detail_size,
                   const char *pass_detail, const char *fail_detail) {
    atlas_verify_check candidate = raw_pass ? ATLAS_CHECK_PASS : ATLAS_CHECK_FAIL;
    atlas_verify_truth meaning = atlas_verify_verifier_truth_of_check(v, candidate);

    if (meaning == ATLAS_TRUTH_ABSENT) {
        const atlas_verify_coverage_dim *dims = NULL;
        size_t count = atlas_verify_verifier_absence_dims(v, &dims);
        atlas_verify_coverage_dim failed = ATLAS_COVDIM_SEMANTIC_GENERATION;
        atlas_verify_truth_reason why = ATLAS_TREASON_COVERAGE_UNKNOWN;
        if (!atlas_verify_coverage_satisfies(cov, dims, count, &failed, &why)) {
            if (check_out != NULL) {
                *check_out = ATLAS_CHECK_UNAVAILABLE;
            }
            /* Recorded on the report so it survives the demotion.
             *
             * Without this the specific reason dies here: `atlas_verify_truth_of`
             * sees only UNAVAILABLE and reports the generic NOT_EVALUATED, so
             * every gated absence would answer §22's "why is this UNKNOWN?"
             * with "no verifier ran" — which is both wrong and the least useful
             * of the possible answers. A model needs to distinguish "the index
             * is stale" from "the address escapes": the first is fixed by
             * reindexing and the second is not fixable at all. */
            cov->reason = why;
            if (detail_out != NULL) {
                /* Names the dimension that fell short and what its
                 * insufficiency means, both from closed Atlas-owned
                 * vocabularies. A reader is told what to go and fix rather than
                 * that something unspecified was wrong. */
                (void)snprintf(detail_out, detail_size,
                               "a negative conclusion here would rest on %s, and %s; Atlas could "
                               "not look rather than looked and found nothing",
                               atlas_verify_coverage_dim_name(failed),
                               atlas_verify_truth_reason_description(why));
            }
            return;
        }
    }

    if (check_out != NULL) {
        *check_out = candidate;
    }
    if (detail_out != NULL) {
        /* Named for the check rather than for the polarity: which of the two is
         * the "found it" sentence depends on the verifier, and each call site
         * supplies them in its own order. */
        (void)snprintf(detail_out, detail_size, "%s", raw_pass ? pass_detail : fail_detail);
    }
}

atlas_status atlas_verify_run_verifier(atlas_db *db, atlas_verify_verifier v, int64_t repo_id,
                                       const char *input, atlas_verify_check *check_out,
                                       atlas_verify_coverage_report *coverage_out, char *scope_out,
                                       size_t scope_size, char *detail_out, size_t detail_size,
                                       atlas_err *err) {
    if (check_out != NULL) {
        *check_out = ATLAS_CHECK_UNAVAILABLE;
    }
    if (scope_out != NULL && scope_size > 0) {
        scope_out[0] = '\0';
    }
    if (detail_out != NULL && detail_size > 0) {
        detail_out[0] = '\0';
    }

    /* The coverage report is always initialised, so a caller that ignores the
     * return path still holds every dimension at UNKNOWN rather than at
     * whatever was on its stack. Every Atlas zero means something, and this
     * one means "nothing was established". */
    atlas_verify_coverage_report local;
    atlas_verify_coverage_report *cov = coverage_out != NULL ? coverage_out : &local;
    atlas_verify_coverage_report_init(cov);
    unobservable_coverage(cov);

    if (db == NULL || v == ATLAS_VERIFIER_NONE) {
        return ATLAS_OK; /* UNAVAILABLE, which is not a fail */
    }

    char arg_a[512];
    char arg_b[512];
    arg_a[0] = '\0';
    arg_b[0] = '\0';

    switch (v) {
    case ATLAS_VERIFIER_CONTENT_HASH: {
        char want[ATLAS_SHA256_HEX_LEN + 1u];
        if (!input_field(input, "path", arg_a, sizeof arg_a) ||
            !input_field(input, "sha256", want, sizeof want)) {
            if (detail_out != NULL) {
                (void)snprintf(detail_out, detail_size,
                               "this verifier needs path= and sha256= and the claim supplies "
                               "neither completely");
            }
            return ATLAS_OK;
        }
        /* A9.2.2. The recorded content is only evidence about the working tree
         * while the file index describes it. With the watcher behind, a
         * mismatch is "Atlas read a stale snapshot", not "the bytes differ" —
         * and this is a different question from A9.2.1's SOURCE_DRIFT, which
         * compares the claim's commit against the scanned head rather than the
         * scanned head against what is on disk. Neither implies the other. */
        bool index_current = false;
        atlas_status ist = atlas_db_verify_index_current(db, repo_id, &index_current, err);
        if (ist != ATLAS_OK) {
            return ist;
        }
        cov->dims[ATLAS_COVDIM_REPOSITORY_SNAPSHOT] =
            index_current ? ATLAS_COVERAGE_COMPLETE : ATLAS_COVERAGE_STALE;

        atlas_buf have = ATLAS_BUF_INIT;
        bool found = false;
        atlas_status st = atlas_db_verify_file_hash(db, repo_id, arg_a, &have, &found, err);
        if (st != ATLAS_OK) {
            atlas_buf_free(&have);
            return st;
        }
        scope_of(v, arg_a, NULL, scope_out, scope_size);
        if (!found) {
            /* The path is not in the index. UNAVAILABLE rather than FAIL: an
             * unscanned or deleted path is not evidence that the bytes differ,
             * and treating it as such would let an index that has not caught up
             * contradict a perfectly true claim. */
            if (detail_out != NULL) {
                (void)snprintf(detail_out, detail_size,
                               "no current content is recorded for that path, so Atlas could not "
                               "look rather than looked and disagreed");
            }
            atlas_buf_free(&have);
            return ATLAS_OK;
        }
        bool match = strcmp(atlas_buf_cstr(&have), want) == 0;
        settle(v, match, cov, check_out, detail_out, detail_size,
               "recorded content hash equals the stated value",
               "recorded content hash differs from the stated value");
        atlas_buf_free(&have);
        return ATLAS_OK;
    }

    case ATLAS_VERIFIER_SYMBOL_PRESENT:
    case ATLAS_VERIFIER_SYMBOL_ABSENT: {
        if (!input_field(input, "symbol", arg_a, sizeof arg_a)) {
            if (detail_out != NULL) {
                (void)snprintf(detail_out, detail_size, "this verifier needs symbol=");
            }
            return ATLAS_OK;
        }
        int64_t count = 0;
        bool complete = false, indexed = false, current = false;
        atlas_status st =
            atlas_db_verify_sem_symbol(db, repo_id, arg_a, &count, &complete, &indexed, err);
        if (st == ATLAS_OK) {
            st = atlas_db_verify_sem_current(db, repo_id, NULL, NULL, &current, err);
        }
        if (st != ATLAS_OK) {
            return st;
        }
        sem_coverage(cov, indexed, complete, current);
        scope_of(v, arg_a, NULL, scope_out, scope_size);
        if (!indexed) {
            /* Recorded so the reason survives: without it `truth_of` sees only
             * UNAVAILABLE and reports the generic NOT_EVALUATED, which tells a
             * caller no verifier ran rather than that there is no index to run
             * one against. Those are different problems with different fixes. */
            cov->reason = ATLAS_TREASON_SEMANTIC_INDEX_ABSENT;
            if (detail_out != NULL) {
                (void)snprintf(detail_out, detail_size,
                               "no semantic generation is published for this repository, so Atlas "
                               "could not look");
            }
            return ATLAS_OK;
        }

        char present_detail[256];
        char absent_detail[256];
        (void)snprintf(present_detail, sizeof present_detail,
                       "%lld matching symbols in the current generation", (long long)count);
        (void)snprintf(absent_detail, sizeof absent_detail,
                       "no symbol of that name across the current generation");

        /* `raw_pass` is each verifier's own truth condition, and `settle()`
         * applies the asymmetry. For SYMBOL_PRESENT the negative direction is
         * FAIL, which A9.2 emitted unguarded and A9.2.2 now gates; for
         * SYMBOL_ABSENT it is PASS, which A9.2 already gated and which now goes
         * through the same one place. */
        bool raw_pass = v == ATLAS_VERIFIER_SYMBOL_PRESENT ? count > 0 : count == 0;
        settle(v, raw_pass, cov, check_out, detail_out, detail_size,
               v == ATLAS_VERIFIER_SYMBOL_PRESENT ? present_detail : absent_detail,
               v == ATLAS_VERIFIER_SYMBOL_PRESENT ? absent_detail : present_detail);
        return ATLAS_OK;
    }

    case ATLAS_VERIFIER_PROVEN_EDGE: {
        if (!input_field(input, "from", arg_a, sizeof arg_a) ||
            !input_field(input, "to", arg_b, sizeof arg_b)) {
            if (detail_out != NULL) {
                (void)snprintf(detail_out, detail_size, "this verifier needs from= and to=");
            }
            return ATLAS_OK;
        }
        bool exists = false, indexed = false, complete = false, current = false;
        atlas_status st = atlas_db_verify_sem_proven_edge(db, repo_id, arg_a, arg_b, &exists,
                                                          &indexed, &complete, err);
        if (st == ATLAS_OK) {
            st = atlas_db_verify_sem_current(db, repo_id, NULL, NULL, &current, err);
        }
        if (st != ATLAS_OK) {
            return st;
        }
        sem_coverage(cov, indexed, complete, current);
        /* This verifier makes no claim about indirect calls, so the dimension
         * is NOT_APPLICABLE rather than UNKNOWN. That distinction matters: a
         * claim must not be blocked by a dimension it does not depend on, and
         * `atlas.no_proven_caller` is the verifier for the wider question. */
        cov->dims[ATLAS_COVDIM_INDIRECT_CALLS] = ATLAS_COVERAGE_NOT_APPLICABLE;
        scope_of(v, arg_a, arg_b, scope_out, scope_size);
        if (!indexed) {
            /* Recorded so the reason survives: without it `truth_of` sees only
             * UNAVAILABLE and reports the generic NOT_EVALUATED, which tells a
             * caller no verifier ran rather than that there is no index to run
             * one against. Those are different problems with different fixes. */
            cov->reason = ATLAS_TREASON_SEMANTIC_INDEX_ABSENT;
            if (detail_out != NULL) {
                (void)snprintf(detail_out, detail_size,
                               "no semantic generation is published for this repository, so Atlas "
                               "could not look");
            }
            return ATLAS_OK;
        }
        /* A9.2.2. A missing proven edge is still a genuine FAIL of *this* claim
         * and still says nothing about indirect calls — but it is only a
         * finding at all when the generation was complete enough to have seen
         * the caller. Over a partial generation the calling translation unit
         * may be exactly the one that did not parse, which A9.2 did not check
         * because it never gathered the flag. */
        settle(v, exists, cov, check_out, detail_out, detail_size,
               "a compiler-proved direct call edge exists; nothing is claimed about calls through "
               "function pointers",
               "no compiler-proved direct call edge is recorded; nothing is claimed about calls "
               "through function pointers");
        return ATLAS_OK;
    }

    case ATLAS_VERIFIER_NO_PROVEN_CALLER: {
        if (!input_field(input, "symbol", arg_a, sizeof arg_a)) {
            if (detail_out != NULL) {
                (void)snprintf(detail_out, detail_size, "this verifier needs symbol=");
            }
            return ATLAS_OK;
        }
        int64_t callers = 0, address_taken = 0;
        bool internal = false, defined = false, complete = false, indexed = false, current = false;
        atlas_status st =
            atlas_db_verify_sem_callers(db, repo_id, arg_a, &callers, &address_taken, &internal,
                                        &defined, &complete, &indexed, err);
        if (st == ATLAS_OK) {
            st = atlas_db_verify_sem_current(db, repo_id, NULL, NULL, &current, err);
        }
        if (st != ATLAS_OK) {
            return st;
        }
        sem_coverage(cov, indexed, complete, current);
        scope_of(v, arg_a, NULL, scope_out, scope_size);

        if (!indexed) {
            /* Recorded so the reason survives: without it `truth_of` sees only
             * UNAVAILABLE and reports the generic NOT_EVALUATED, which tells a
             * caller no verifier ran rather than that there is no index to run
             * one against. Those are different problems with different fixes. */
            cov->reason = ATLAS_TREASON_SEMANTIC_INDEX_ABSENT;
            if (detail_out != NULL) {
                (void)snprintf(detail_out, detail_size,
                               "no semantic generation is published for this repository, so Atlas "
                               "could not look");
            }
            return ATLAS_OK;
        }
        if (!defined) {
            /* Nothing calls a function that does not exist, and saying so would
             * be true and useless — worse, it would answer a question about the
             * wrong subject with a confident absence. UNAVAILABLE. */
            if (detail_out != NULL) {
                (void)snprintf(detail_out, detail_size,
                               "no symbol of that name is defined in the current generation, so "
                               "there is nothing whose callers could be enumerated");
            }
            return ATLAS_OK;
        }

        /* --- the two coverage dimensions that make this verifier possible ---
         *
         * INDIRECT_CALLS: a C function cannot be reached through a pointer, a
         * dispatch table, a callback or a dynamic registration unless its
         * address is taken somewhere. `ADDRESS_TAKEN` is a PROVEN edge naming
         * the function, so zero of them over a *complete* generation excludes
         * every one of those mechanisms at once — a stronger and far more
         * checkable statement than enumerating them. Where the address does
         * escape, no amount of further indexing recovers the target set, so the
         * dimension is PARTIAL and the answer will be UNKNOWN.
         *
         * The generation must be complete for the count itself to be worth
         * anything: a translation unit that failed to parse could hold the
         * address-take. That is why this is gated on `complete` rather than
         * read on its own. */
        cov->dims[ATLAS_COVDIM_INDIRECT_CALLS] =
            (complete && address_taken == 0) ? ATLAS_COVERAGE_COMPLETE : ATLAS_COVERAGE_PARTIAL;

        /* EXTERNAL_CALLERS: an internal-linkage symbol cannot be named from
         * outside its own translation unit, so the indexed tree is the whole
         * world for it and the dimension is NOT_APPLICABLE — there is nothing
         * out there that could call it. An external one can be called from code
         * Atlas never indexed and reached through `dlsym`, neither of which any
         * amount of indexing would reveal, so it stays PARTIAL and the claim
         * comes back UNKNOWN. Bounding it mechanically beats a caveat. */
        cov->dims[ATLAS_COVDIM_EXTERNAL_CALLERS] =
            internal ? ATLAS_COVERAGE_NOT_APPLICABLE : ATLAS_COVERAGE_PARTIAL;

        char found_detail[256];
        (void)snprintf(found_detail, sizeof found_detail,
                       "%lld compiler-proved direct callers, and the address is taken %lld times",
                       (long long)callers, (long long)address_taken);

        settle(v, callers == 0, cov, check_out, detail_out, detail_size,
               "no compiler-proved direct caller, no address-take, and internal linkage across a "
               "complete generation",
               found_detail);
        return ATLAS_OK;
    }

    case ATLAS_VERIFIER_NONE:
        break;
    }
    return ATLAS_OK;
}

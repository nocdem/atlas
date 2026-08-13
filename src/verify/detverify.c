/* Atlas - A9.2: the deterministic verifiers.
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
 * The practical consequence is worth stating plainly: Atlas can mechanically
 * establish facts about *what is recorded in its own index* — bytes at a
 * commit, symbols and proven edges in a semantic generation — and cannot
 * mechanically establish facts that require running the software. The second
 * kind is empirical here, and empirical is in shadow.
 *
 * ## UNAVAILABLE is not FAIL
 *
 * The single most dangerous confusion available to this file. An index that has
 * not run cannot establish that a symbol is absent; reporting "could not look"
 * as "it is not there" is how a remediation detector closes an obligation that
 * is still outstanding. Every verifier below returns UNAVAILABLE rather than
 * FAIL when it could not look, and the auto-lifecycle engine treats UNAVAILABLE
 * as a blocking reason rather than as evidence of anything.
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
    case ATLAS_VERIFIER_NONE:
        (void)snprintf(out, out_size, "nothing");
        break;
    }
}

atlas_status atlas_verify_run_verifier(atlas_db *db, atlas_verify_verifier v, int64_t repo_id,
                                       const char *input, atlas_verify_check *check_out,
                                       char *scope_out, size_t scope_size, char *detail_out,
                                       size_t detail_size, atlas_err *err) {
    if (check_out != NULL) {
        *check_out = ATLAS_CHECK_UNAVAILABLE;
    }
    if (scope_out != NULL && scope_size > 0) {
        scope_out[0] = '\0';
    }
    if (detail_out != NULL && detail_size > 0) {
        detail_out[0] = '\0';
    }
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
        if (check_out != NULL) {
            *check_out = match ? ATLAS_CHECK_PASS : ATLAS_CHECK_FAIL;
        }
        if (detail_out != NULL) {
            (void)snprintf(detail_out, detail_size, "recorded content hash %s the stated value",
                           match ? "equals" : "differs from");
        }
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
        bool complete = false, indexed = false;
        atlas_status st =
            atlas_db_verify_sem_symbol(db, repo_id, arg_a, &count, &complete, &indexed, err);
        if (st != ATLAS_OK) {
            return st;
        }
        scope_of(v, arg_a, NULL, scope_out, scope_size);
        if (!indexed) {
            if (detail_out != NULL) {
                (void)snprintf(detail_out, detail_size,
                               "no semantic generation is published for this repository, so Atlas "
                               "could not look");
            }
            return ATLAS_OK;
        }
        if (v == ATLAS_VERIFIER_SYMBOL_PRESENT) {
            /* Presence is establishable over a partial index: finding it is
             * finding it, and an incomplete generation cannot conjure a symbol
             * that is not there. The asymmetry with absence is the point. */
            if (check_out != NULL) {
                *check_out = count > 0 ? ATLAS_CHECK_PASS : ATLAS_CHECK_FAIL;
            }
            if (detail_out != NULL) {
                (void)snprintf(detail_out, detail_size, "%lld matching symbols in the current "
                                                        "generation",
                               (long long)count);
            }
            return ATLAS_OK;
        }
        /* Absence. **Only a complete generation can establish it.** A
         * generation with failed, partial or unsupported translation units
         * describes part of a repository, and "I did not find it" over part of
         * a repository is not "it is not there" — which, on the obligation
         * remediation path, is the difference between closing a discharged
         * blocker and closing an outstanding one. */
        if (!complete) {
            if (detail_out != NULL) {
                (void)snprintf(detail_out, detail_size,
                               "the current semantic generation is not complete, and an absence "
                               "cannot be established over part of a repository");
            }
            return ATLAS_OK;
        }
        if (check_out != NULL) {
            *check_out = count == 0 ? ATLAS_CHECK_PASS : ATLAS_CHECK_FAIL;
        }
        if (detail_out != NULL) {
            (void)snprintf(detail_out, detail_size,
                           "%lld matching symbols across a complete generation",
                           (long long)count);
        }
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
        bool exists = false, indexed = false;
        atlas_status st =
            atlas_db_verify_sem_proven_edge(db, repo_id, arg_a, arg_b, &exists, &indexed, err);
        if (st != ATLAS_OK) {
            return st;
        }
        scope_of(v, arg_a, arg_b, scope_out, scope_size);
        if (!indexed) {
            if (detail_out != NULL) {
                (void)snprintf(detail_out, detail_size,
                               "no semantic generation is published for this repository, so Atlas "
                               "could not look");
            }
            return ATLAS_OK;
        }
        /* A missing proven edge is a genuine FAIL of *this* claim and says
         * nothing about indirect calls. Atlas never claims to know every target
         * of a function pointer — A8-CI's rule — and the scope sentence above
         * says "direct" for exactly that reason. */
        if (check_out != NULL) {
            *check_out = exists ? ATLAS_CHECK_PASS : ATLAS_CHECK_FAIL;
        }
        if (detail_out != NULL) {
            (void)snprintf(detail_out, detail_size,
                           "a compiler-proved direct call edge %s; nothing is claimed about calls "
                           "through function pointers",
                           exists ? "exists" : "is not recorded");
        }
        return ATLAS_OK;
    }

    case ATLAS_VERIFIER_NONE:
        break;
    }
    return ATLAS_OK;
}

/* Atlas - building and publishing a semantic generation.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The shape of the pass, and why each step is where it is:
 *
 *   1. **Read the compilation databases.** Named by the caller, never
 *      discovered. Each is opened without following a symlink, bounded, and
 *      parsed by A3's `atlas_code_compdb_parse`, which already reduces every
 *      entry to a positive allowlist and drops the `command` string after
 *      hashing it. Nothing executable survives that step, which is why this
 *      file has no argument handling of its own.
 *   2. **Decide, per unit, parse or carry forward.** The comparison is the
 *      unit's *input digest*: its own content hash, the content hash of every
 *      file it included last time, and its configuration digest. Unchanged
 *      means genuinely unchanged, including through a header three levels down.
 *   3. **Parse, one bounded child at a time.** Between transactions, never
 *      inside one, and never from a worker job — A1's rules, unchanged.
 *   4. **Apply in batches.** `ATLAS_SEM_APPLY_BATCH` rows per transaction, so no
 *      write transaction is ever held across unbounded work.
 *   5. **Attach candidates.** Once, at the end, because it is the only step
 *      that needs the whole repository: a call site in one unit is answered by
 *      an address taken in another.
 *   6. **Publish.** One transaction that marks the generation COMPLETE and
 *      repoints `sem_current`. Before this statement no reader can see any of
 *      the above; after it, all of it. There is no moment in between.
 *
 * A failure at any point leaves the generation FAILED and the previous one
 * current and complete. A crash leaves it RUNNING and unpublished, which the
 * next pass reports and reaps — the durable record says what happened rather
 * than what was intended, which is A8's argument for `resolve_settled`.
 */
#include "atlas/sem.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/db.h"
#include "atlas/sem_discover.h"
#include "atlas/sem_ops.h"
#include "atlas/pathrep.h"
#include "atlas/sha256.h"
#include "atlas/syspolicy.h"

void atlas_sem_index_opts_init(atlas_sem_index_opts *o) {
    memset(o, 0, sizeof(*o));
    o->root_fd = -1;
}

void atlas_sem_index_summary_init(atlas_sem_index_summary *s) { memset(s, 0, sizeof(*s)); }

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* --- freshness --------------------------------------------------------------
 *
 * Recomputed, never stored. The order of the checks is the order an operator
 * would want to be told about them: the repository moved, then the build
 * description moved, then the tools moved. */
atlas_sem_freshness atlas_sem_freshness_of(const atlas_sem_generation *g, bool have_generation,
                                           bool running, const char *live_commit,
                                           const char *live_repo_identity,
                                           const char *live_compdb_digest,
                                           const char *live_source_identity,
                                           const atlas_sem_live_inputs *live_inputs,
                                           bool file_index_current, const char **reason_out) {
    if (reason_out != NULL) {
        *reason_out = NULL;
    }
    if (!have_generation) {
        /* ABSENT is not STALE. "Nobody has ever indexed this" and "what was
         * indexed no longer describes the code" are different answers and an
         * operator does different things about them. */
        return ATLAS_SEM_FRESH_ABSENT;
    }
    if (g->status != ATLAS_SEM_GEN_COMPLETE) {
        if (reason_out != NULL) {
            *reason_out = ATLAS_SEM_STALE_INCOMPLETE;
        }
        return ATLAS_SEM_FRESH_STALE;
    }
    /* A9.2.5. A different repository outranks a different commit of the same one.
     *
     * Guarded by the same empty-value rule every other check here uses: an empty
     * stored identity is a generation built before this was recorded, and an
     * empty live one is Atlas not having looked. Neither is evidence of change,
     * and treating either as one would make every pre-A9.2.5 generation stale
     * for a reason nobody could act on. */
    if (live_repo_identity != NULL && live_repo_identity[0] != '\0' &&
        g->repo_identity_hash[0] != '\0' &&
        strcmp(live_repo_identity, g->repo_identity_hash) != 0) {
        if (reason_out != NULL) {
            *reason_out = ATLAS_SEM_STALE_REPO_IDENTITY;
        }
        return ATLAS_SEM_FRESH_STALE;
    }
    if (live_commit != NULL && live_commit[0] != '\0' && g->commit_id[0] != '\0' &&
        strcmp(live_commit, g->commit_id) != 0) {
        if (reason_out != NULL) {
            *reason_out = ATLAS_SEM_STALE_COMMIT;
        }
        return ATLAS_SEM_FRESH_STALE;
    }
    if (live_compdb_digest != NULL && live_compdb_digest[0] != '\0' &&
        g->compdb_digest[0] != '\0' && strcmp(live_compdb_digest, g->compdb_digest) != 0) {
        if (reason_out != NULL) {
            *reason_out = ATLAS_SEM_STALE_COMPDB;
        }
        return ATLAS_SEM_FRESH_STALE;
    }
    if (g->compiler_version[0] != '\0' &&
        strcmp(g->compiler_version, atlas_sem_compiler_version()) != 0) {
        if (reason_out != NULL) {
            *reason_out = ATLAS_SEM_STALE_COMPILER;
        }
        return ATLAS_SEM_FRESH_STALE;
    }
    if (strcmp(g->analyzer_id, ATLAS_SEM_ANALYZER_ID) != 0 ||
        g->analyzer_version != ATLAS_SEM_ANALYZER_VERSION) {
        /* The graph was produced by a different algorithm. The bytes and the
         * compilation database may be identical and the answers still wrong in
         * exactly the way the upgrade fixed — A3's argument for the analyzer
         * epoch, and it applies unchanged here. */
        if (reason_out != NULL) {
            *reason_out = ATLAS_SEM_STALE_ANALYZER;
        }
        return ATLAS_SEM_FRESH_STALE;
    }
    if (!file_index_current) {
        /* A semantic graph built on a file index nobody can vouch for is not a
         * graph anybody should act on — `atlas_code_index_current`'s rule. */
        if (reason_out != NULL) {
            *reason_out = ATLAS_SEM_STALE_FILE_INDEX;
        }
        return ATLAS_SEM_FRESH_STALE;
    }
    /* A9.2.4, and placed here — after everything specific, before the broadest
     * check — for the reason the ordering exists at all: an operator should be
     * told *why* in the most useful terms available.
     *
     * "The set of build descriptions changed" and "the working tree changed" are
     * both true when a new compilation database appears, and only the first
     * tells anybody what happened. Without this branch a new build directory
     * would report `the_repository_moved_since_this_index_was_built`, which is
     * accurate, useless, and would send somebody looking at their source.
     *
     * A `known` flag rather than a sentinel count, because zero accepted inputs
     * is a real and meaningful state: a repository whose build description
     * vanished has *fewer* inputs than its generation, and that is exactly the
     * case this must catch rather than mistake for "nothing was measured". */
    if (live_inputs != NULL && live_inputs->known && g->discovery != ATLAS_SEM_DISC_UNKNOWN &&
        (live_inputs->accepted_count != g->input_count ||
         live_inputs->discovery != g->discovery)) {
        if (reason_out != NULL) {
            *reason_out = ATLAS_SEM_STALE_DISCOVERY;
        }
        return ATLAS_SEM_FRESH_STALE;
    }
    /* A9.2.3, and it is last on purpose: it is the broadest check and the least
     * specific answer.
     *
     * Every check above compares something that moves with a *commit*. Atlas
     * indexes the working tree, so a source could be edited, added or deleted
     * with the head standing still and all four of them would agree the index
     * was current — which is exactly the state a developer is in for most of a
     * working day, and the state the daemon has to notice.
     *
     * Ordered after the others so a repository that has moved on is told *why*
     * in the most useful terms available, and falls back to "the working tree
     * changed" only when nothing more specific applies.
     *
     * An empty stored identity does not make a generation stale. A generation
     * built before this season recorded nothing to compare, and "this index did
     * not record what it was built from" is not evidence that the tree has
     * moved. It is rebuilt on its next automatic pass and records one. */
    if (live_source_identity != NULL && live_source_identity[0] != '\0' &&
        g->source_identity[0] != '\0' && strcmp(live_source_identity, g->source_identity) != 0) {
        if (reason_out != NULL) {
            *reason_out = ATLAS_SEM_STALE_SOURCE;
        }
        return ATLAS_SEM_FRESH_STALE;
    }
    if (running) {
        return ATLAS_SEM_FRESH_REBUILDING;
    }
    return ATLAS_SEM_FRESH_CURRENT;
}

/* Every live fact a freshness comparison needs, gathered in one pass.
 *
 * The compilation databases feed two digests — the compdb-list digest that gives
 * the specific "a compilation database changed" reason, and the discovery digest
 * that feeds the source identity — and reading their bytes once for both is why
 * this struct exists. See the comment at the call site. */
typedef struct live_fact_set {
    char discovery_digest[65];
    char compdb_digest[65];
    char source_identity[65];
    atlas_sem_live_inputs inputs;
    /* A9.2.5. Two more facts the same pass already has in hand, carried out
     * rather than fetched again: `atlas_sem_trust_now` needs the rejected count
     * and the operator's activation intent, and re-reading the build description
     * for them would put a second config read on every semantic query — the
     * shape A9.2.3's closure measured and removed. */
    int64_t inputs_rejected;
    atlas_sem_auto_intent auto_intent;
    /* A9.2.5. The repository's *current* lineage fingerprint, so freshness can
     * notice that a generation describes a different repository. */
    char repo_identity[65];
} live_fact_set;

static atlas_status live_facts(atlas_db *db, atlas_repo_info *repo, live_fact_set *out,
                               atlas_err *err);

atlas_sem_freshness atlas_sem_freshness_now(atlas_db *db, atlas_repo_info *repo,
                                            const atlas_sem_generation *g, bool have_generation,
                                            bool running, const char **reason_out) {
    /* The file index has to be current too: a semantic graph built on a file
     * index nobody can vouch for is not one to act on. Asked through
     * `atlas_index_state_is_current`, which is the single authority on that
     * question — A9.2.2 moved it out of the serve loop so exactly this kind of
     * caller could ask it rather than restate it. */
    atlas_index_state fs;
    atlas_index_state_init(&fs);
    atlas_err ignored;
    atlas_err_init(&ignored);
    bool file_current = atlas_db_index_state_get(db, repo->id, &fs, &ignored) == ATLAS_OK &&
                        atlas_index_state_is_current(&fs, NULL);
    atlas_index_state_free(&fs);

    /* Every live fact, from **one** pass over the build description.
     *
     * Best effort throughout, and every one of them fails *open* on purpose: an
     * empty live value never makes a generation stale, because "Atlas could not
     * look" is not evidence that anything changed. A repository whose build
     * description cannot be read is reported by the index attempt, where the
     * failure has somewhere to go.
     *
     * One pass rather than three, and that is a correctness and a cost argument
     * in one. A9.2.3's closure measured this exact shape: `sem.status` computed
     * freshness twice per response and hashed every declared compilation
     * database twice, and two computations within one document could also have
     * disagreed if the tree moved between them. A9.2.4 would have made it worse
     * — the compilation databases feed *two* digests now — so `live_facts`
     * reads each one once and derives both from the same bytes. */
    live_fact_set lf;
    (void)live_facts(db, repo, &lf, &ignored);
    const char *live_digest = lf.compdb_digest;
    const char *live_identity = lf.source_identity;
    atlas_sem_live_inputs live_inputs = lf.inputs;

    return atlas_sem_freshness_of(g, have_generation, running, repo->scanned_head,
                                  lf.repo_identity, live_digest, live_identity, &live_inputs,
                                  file_current, reason_out);
}

/* A9.2.5: every trust fact a semantic read must carry, from **one** pass.
 *
 * This replaces `atlas_sem_freshness_now` on the query paths rather than joining
 * it, and that is the whole cost argument: `live_facts` already reads the build
 * description, the accepted inputs and every source hash, and it is already
 * called once per semantic response. Adding the verdict therefore costs the
 * `atlas_db_sem_current`-shaped reads the caller has already done plus nothing —
 * no second source-identity computation, no second config read.
 *
 * `atlas_sem_plan_for` is deliberately **not** called here even though it holds
 * the same fields. It loads the root-owned policy and computes a scheduling
 * decision, and a query has no business asking whether a rebuild is due; the
 * activation answer it does need arrives as `policy_default` from a caller who
 * already has it.
 *
 * The verdict is left unsettled: only the caller knows how many rows it emitted
 * and whether its walk was truncated, and `atlas_sem_trust_settle` is the one
 * function that decides. */
void atlas_sem_trust_now_with_default(atlas_db *db, atlas_repo_info *repo,
                                      const atlas_sem_generation *g, bool have_generation,
                                      bool running, bool policy_default, atlas_sem_trust *out) {
    if (out == NULL) {
        return;
    }
    atlas_sem_trust_init(out);
    out->libclang_available = atlas_sem_available();
    if (db == NULL || repo == NULL) {
        return;
    }

    atlas_index_state fs;
    atlas_index_state_init(&fs);
    atlas_err ignored;
    atlas_err_init(&ignored);
    bool file_current = atlas_db_index_state_get(db, repo->id, &fs, &ignored) == ATLAS_OK &&
                        atlas_index_state_is_current(&fs, NULL);
    atlas_index_state_free(&fs);

    live_fact_set lf;
    (void)live_facts(db, repo, &lf, &ignored);

    const char *reason = NULL;
    out->freshness = atlas_sem_freshness_of(g, have_generation, running, repo->scanned_head,
                                            lf.repo_identity, lf.compdb_digest, lf.source_identity,
                                            &lf.inputs, file_current, &reason);
    out->stale_reason = atlas_sem_stale_reason_intern(reason);

    out->have_generation = have_generation;
    (void)snprintf(out->live_identity, sizeof out->live_identity, "%s", lf.source_identity);
    out->discovery = lf.inputs.discovery;
    out->inputs_accepted = lf.inputs.accepted_count;
    out->inputs_rejected = lf.inputs_rejected;
    /* The whole activation policy in one pure function, so this surface and the
     * scheduler answer identically because they call it rather than because two
     * copies are kept in step. */
    out->auto_maintenance = atlas_sem_auto_effective(lf.auto_intent, policy_default);

    if (have_generation && g != NULL) {
        out->generation_id = g->id;
        (void)snprintf(out->indexed_commit, sizeof out->indexed_commit, "%s", g->commit_id);
        (void)snprintf(out->generation_identity, sizeof out->generation_identity, "%s",
                       g->source_identity);
        out->scope_discovery = g->scope_discovery;
        out->scope_candidates = g->scope_candidates;
        out->scope_covered = g->scope_covered;
        out->scope_uncovered = g->scope_uncovered;
        out->generation_discovery = g->discovery;
        out->units_complete =
            g->tu_partial == 0 && g->tu_failed == 0 && g->tu_unsupported == 0;
        /* Asked, not restated. An earlier cut open-coded the same four
         * conditions here and conceded in a comment that "the two must agree" —
         * which is exactly the second copy `atlas_sem_coverage_gap` exists to
         * remove, and which made `docs/engineering-rules.md`'s claim that all
         * three callers ask one function untrue. */
        out->coverage_complete =
            atlas_sem_coverage_gap(g->scope_discovery, g->discovery, out->units_complete,
                                   g->scope_uncovered) == NULL;
    }
}

void atlas_sem_trust_now(atlas_db *db, atlas_repo_info *repo, const atlas_sem_generation *g,
                         bool have_generation, bool running, atlas_sem_trust *out) {
    atlas_syspolicy pol;
    atlas_syspolicy_load(&pol);
    atlas_sem_trust_now_with_default(db, repo, g, have_generation, running,
                                     atlas_syspolicy_semantic_auto_default(&pol), out);
}

/* --- reading a compilation database ---------------------------------------- */

typedef struct compdb_slot {
    atlas_buf rel;  /* repository-relative path, as the operator named it */
    atlas_buf data; /* the document bytes */
    char digest[65];
    atlas_code_compdb parsed;
    int64_t row_id;
} compdb_slot;

static void slot_free(compdb_slot *s) {
    atlas_buf_free(&s->rel);
    atlas_buf_free(&s->data);
    atlas_code_compdb_free(&s->parsed);
}

/* Reads one compilation database from inside the repository.
 *
 * `atlas_path_open_nofollow` refuses to traverse a symlink, so a repository
 * cannot point this at `/etc/shadow` by planting a link where a build directory
 * is expected. A document that is missing, oversized or malformed produces zero
 * units and a recorded reason — an ordinary outcome, not a failure that takes
 * the pass down, which is how A3's reader behaves and for the same reason. */
static atlas_status read_bounded(int root_fd, const char *rel, size_t max, atlas_buf *out,
                                 atlas_err *err) {
    atlas_path_open_result res = ATLAS_PATH_OPEN_MISSING;
    int fd = -1;
    struct stat sb;
    memset(&sb, 0, sizeof(sb));
    atlas_status st = atlas_path_open_nofollow(root_fd, rel, strlen(rel), &res, &fd, &sb, NULL, err);
    if (st != ATLAS_OK) {
        if (fd >= 0) {
            (void)close(fd);
        }
        return st;
    }
    if (res != ATLAS_PATH_OPEN_OK) {
        if (fd >= 0) {
            (void)close(fd);
        }
        /* Includes the symlinked case: a component that is a link refuses the
         * open rather than being followed, so a repository cannot point this at
         * a file outside itself by planting one where a build directory is
         * expected. */
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "the named compilation database is not a readable regular file "
                             "inside the repository");
    }
    for (;;) {
        char chunk[64u * 1024u];
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            st = atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno,
                                     "cannot read the compilation database");
            break;
        }
        if (n == 0) {
            break;
        }
        if (out->len + (size_t)n > max) {
            /* Refused, never truncated: half a compilation database describes a
             * different repository, and nothing in the result would say so. */
            st = atlas_err_set(err, ATLAS_ERR_USAGE,
                               "the compilation database exceeds %zu bytes", max);
            break;
        }
        st = atlas_buf_append(out, chunk, (size_t)n, err);
        if (st != ATLAS_OK) {
            break;
        }
    }
    (void)close(fd);
    return st;
}

static atlas_status load_compdb(int root_fd, const char *origin, const char *rel,
                                compdb_slot *slot, atlas_err *err) {
    atlas_buf_init(&slot->rel);
    atlas_buf_init(&slot->data);
    atlas_code_compdb_init(&slot->parsed);
    slot->digest[0] = '\0';

    atlas_status st = atlas_buf_set_str(&slot->rel, rel, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = read_bounded(root_fd, rel, ATLAS_CODE_MAX_COMPILE_DB_BYTES, &slot->data, err);
    if (st != ATLAS_OK) {
        return st;
    }
    atlas_sha256_hex(slot->data.data, slot->data.len, slot->digest);
    /* Parsed against the root the database was written for, not the root its
     * bytes were read from. See `origin_root`. */
    return atlas_code_compdb_parse(slot->data.data, slot->data.len, origin, strlen(origin),
                                   &slot->parsed, err);
}

/* The digest a generation is judged stale against: every database's own digest,
 * in the order the operator named them, domain-separated and length-prefixed for
 * A4's reason. */
static void digest_list(const char (*digests)[65], size_t n, char out[65]) {
    atlas_sha256 h;
    atlas_sha256_init(&h);
    static const char DOMAIN[] = "atlas.sem.compdbs.v1";
    unsigned char len[8];
    for (size_t i = 0; i <= n; i++) {
        const char *s = i == 0 ? DOMAIN : digests[i - 1];
        uint64_t sn = strlen(s);
        for (int b = 0; b < 8; b++) {
            len[b] = (unsigned char)((sn >> (8 * (7 - b))) & 0xffu);
        }
        atlas_sha256_update(&h, len, sizeof(len));
        atlas_sha256_update(&h, s, (size_t)sn);
    }
    unsigned char digest[ATLAS_SHA256_DIGEST_LEN];
    atlas_sha256_final(&h, digest);
    atlas_hex_encode(digest, sizeof(digest), out);
    out[ATLAS_SHA256_HEX_LEN] = '\0';
}

/* Domain-separated and length-prefixed, for A4's reason: with any single-byte
 * delimiter two different lists could encode identically, and this one decides
 * whether a repository is rebuilt. */
static void feed_str(atlas_sha256 *h, const char *s) {
    uint64_t n = s == NULL ? 0 : (uint64_t)strlen(s);
    unsigned char len[8];
    for (int i = 0; i < 8; i++) {
        len[i] = (unsigned char)((n >> (8 * (7 - i))) & 0xffu);
    }
    atlas_sha256_update(h, len, sizeof(len));
    if (n > 0) {
        atlas_sha256_update(h, s, (size_t)n);
    }
}

static void digest_all(const compdb_slot *slots, size_t n, char out[65]) {
    char digests[ATLAS_SEM_MAX_COMPDBS][65];
    for (size_t i = 0; i < n && i < ATLAS_SEM_MAX_COMPDBS; i++) {
        (void)snprintf(digests[i], sizeof digests[i], "%s", slots[i].digest);
    }
    digest_list((const char (*)[65])digests, n, out);
}

/* --- the three public live values, each a thin read of one shared pass -------
 *
 * All three are `live_facts`, and that is the whole point: the compilation
 * databases feed two digests and the source identity folds the second, so three
 * separate implementations would read the same bytes three times *and* be three
 * chances for the stored and the live form of one value to be computed
 * differently. A9.2.3's closure fixed exactly that shape once already. */

atlas_status atlas_sem_repo_compdb_digest(atlas_db *db, atlas_repo_info *repo, char out[65],
                                          atlas_err *err) {
    out[0] = '\0';
    live_fact_set lf;
    atlas_status st = live_facts(db, repo, &lf, err);
    if (st == ATLAS_OK) {
        (void)snprintf(out, 65, "%s", lf.compdb_digest);
    }
    return st;
}

atlas_status atlas_sem_repo_discovery_identity(atlas_db *db, atlas_repo_info *repo, char out[65],
                                               atlas_err *err) {
    out[0] = '\0';
    live_fact_set lf;
    atlas_status st = live_facts(db, repo, &lf, err);
    if (st == ATLAS_OK) {
        (void)snprintf(out, 65, "%s", lf.discovery_digest);
    }
    return st;
}

atlas_status atlas_sem_source_identity(atlas_db *db, atlas_repo_info *repo, char out[65],
                                       atlas_err *err) {
    out[0] = '\0';
    live_fact_set lf;
    atlas_status st = live_facts(db, repo, &lf, err);
    if (st == ATLAS_OK) {
        (void)snprintf(out, 65, "%s", lf.source_identity);
    }
    return st;
}

/* A9.2.4. Walks a repository for compilation databases and records what it
 * found, in one transaction.
 *
 * The walk itself is `atlas_sem_discover`, which touches no database; this is
 * the layer that makes its result durable, and it is deliberately the only one.
 * The delete and the inserts are one fact — a candidate list half replaced is a
 * search nobody performed — so the transaction is opened here rather than left
 * to a caller who might reasonably forget.
 *
 * A1's rule that no file read happens inside a write transaction is why the walk
 * runs to completion *before* `atlas_db_begin`. */
atlas_status atlas_sem_discovery_run(atlas_db *db, atlas_repo_info *repo,
                                     void (*yield)(void *ud), void *yield_ud,
                                     atlas_sem_discovery_result *out, atlas_err *err) {
    if (db == NULL || repo == NULL || out == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "build-input discovery: bad request");
    }
    atlas_sem_config cfg;
    atlas_sem_config_init(&cfg);
    atlas_status st = atlas_db_sem_config_get(db, repo->id, &cfg, err);
    if (st != ATLAS_OK) {
        atlas_sem_config_free(&cfg);
        return st;
    }

    /* The walk, with whatever the caller gave it to lend its thread to. It runs
     * to completion before `atlas_db_begin` below, so no yield it makes can
     * happen inside a transaction — A1's rule, and the reason this ordering was
     * already what it is. */
    st = atlas_sem_discover(atlas_buf_cstr(&repo->root_path), &cfg, yield, yield_ud, out, err);
    if (st != ATLAS_OK) {
        /* Atlas could not look at all — an unreadable or replaced root. The
         * stored verdict is left alone rather than overwritten with UNKNOWN:
         * "I could not look this time" is not evidence that what was found last
         * time is wrong, and blanking it would turn a transient failure into a
         * repository that cannot support an absence until somebody notices. */
        atlas_sem_config_free(&cfg);
        return st;
    }

    st = atlas_db_begin(db, err);
    if (st == ATLAS_OK) {
        st = atlas_db_sem_obstacles_replace(db, repo->id, out->obstacles, out->obstacle_count,
                                            out->discovered_at, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_sem_inputs_replace(db, repo->id, out->inputs, out->count, out->discovered_at,
                                         err);
    }
    if (st == ATLAS_OK) {
        /* Best effort on the durable identity: it is what lets a row still say
         * which repository lineage it described after the rowid is gone, and a
         * discovery result is worth recording even when that lookup fails. */
        atlas_buf identity = ATLAS_BUF_INIT;
        atlas_err ignored;
        atlas_err_init(&ignored);
        (void)atlas_db_repo_identity_hash(db, repo->id, &identity, &ignored);
        st = atlas_db_sem_discovery_set(db, repo->id, atlas_buf_cstr(&identity),
                                        atlas_sem_discovery_name(out->state), out->discovered_at,
                                        out->limit_reached ? out->limit_detail : "", err);
        atlas_buf_free(&identity);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_commit(db, err);
    } else {
        atlas_db_rollback(db);
    }
    atlas_sem_config_free(&cfg);
    return st;
}

/* The accepted set as the indexer takes it: NUL-separated, repository-relative,
 * in path order.
 *
 * Read from the persisted candidate list rather than from the operator's pinned
 * list, which is the whole difference between A9.2.3 and this season: what gets
 * indexed is what discovery accepted, and a pinned path is one input to that
 * rather than the entirety of it. */
atlas_status atlas_sem_accepted_inputs(atlas_db *db, int64_t repo_id, atlas_buf *out,
                                       size_t *count_out, atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    if (out == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "accepted build inputs: bad request");
    }
    atlas_sem_input *inputs = calloc(ATLAS_SEM_DISCOVERY_MAX_CANDIDATES, sizeof(*inputs));
    if (inputs == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory reading the build inputs");
    }
    size_t count = 0;
    atlas_status st = atlas_db_sem_inputs_get(db, repo_id, inputs,
                                              ATLAS_SEM_DISCOVERY_MAX_CANDIDATES, &count, err);
    for (size_t i = 0; st == ATLAS_OK && i < count; i++) {
        if (!inputs[i].accepted) {
            continue;
        }
        st = atlas_buf_append(out, inputs[i].path, strlen(inputs[i].path), err);
        if (st == ATLAS_OK) {
            st = atlas_buf_append_ch(out, '\0', err);
        }
        if (st == ATLAS_OK && count_out != NULL) {
            (*count_out)++;
        }
    }
    free(inputs);
    return st;
}

/* The source identity's final fold, given the two digests it composes.
 *
 * One composer, so the stored and the live form of an identity can never differ
 * about the domain or the order — which would make a generation's recorded
 * identity incomparable with a freshly computed one and report a change nobody
 * made.
 *
 * The domain bumped from v1 with A9.2.4, because what the identity covers
 * changed: it now folds the whole *input universe* rather than a digest over the
 * compilation databases an operator happened to name. Every identity stored
 * before this season means something different from one computed now, so the
 * bump makes every pre-A9.2.4 generation stale exactly once — the honest
 * outcome, since those generations were built without knowing whether their
 * input set was complete.
 *
 * The **content hashes** in `content_digest` are what make this the working-tree
 * answer rather than a commit answer: Atlas indexes the tree it can see, and an
 * uncommitted edit changes a hash while every commit-derived value stands
 * still. */
static void source_identity_from(const char *discovery_digest, const char *content_digest,
                                 char out[65]) {
    atlas_sha256 h;
    atlas_sha256_init(&h);
    feed_str(&h, "atlas.sem.source-identity.v2");
    feed_str(&h, discovery_digest);
    feed_str(&h, atlas_sem_compiler_version());
    feed_str(&h, ATLAS_SEM_ANALYZER_ID);
    char av[32];
    (void)snprintf(av, sizeof av, "%d", ATLAS_SEM_ANALYZER_VERSION);
    feed_str(&h, av);
    /* Every live C source and header, by path and content hash, in path order —
     * digested in `src/db`, because sqlite3 types do not leave that layer.
     * Headers as well as sources: a header edit changes what every unit
     * including it compiles to, and an identity that ignored headers would
     * report a repository unchanged after an edit that changes half its graph. */
    feed_str(&h, content_digest);

    unsigned char digest[ATLAS_SHA256_DIGEST_LEN];
    atlas_sha256_final(&h, digest);
    atlas_hex_encode(digest, sizeof(digest), out);
    out[ATLAS_SHA256_HEX_LEN] = '\0';
}

/* One pass over the build description, producing every live value a freshness
 * comparison needs.
 *
 * The reads it does *not* repeat are the point. A9.2.3's closure commit measured
 * `sem.status` computing freshness twice and hashing every declared compilation
 * database twice, and fixed it; A9.2.4 gives the same bytes a second consumer —
 * the discovery digest that feeds the source identity — so without this the
 * regression would have come back larger. Each accepted database is opened,
 * read and hashed exactly once, and both digests are derived from that hash.
 *
 * Fails open at every step: an unreadable root, an unreadable database or a
 * missing candidate list leaves the corresponding value empty, and an empty live
 * value never makes a generation stale. */
static atlas_status live_facts(atlas_db *db, atlas_repo_info *repo, live_fact_set *out,
                               atlas_err *err) {
    memset(out, 0, sizeof(*out));
    if (db == NULL || repo == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "live semantic facts: bad request");
    }

    atlas_sem_config cfg;
    atlas_sem_config_init(&cfg);
    atlas_status st = atlas_db_sem_config_get(db, repo->id, &cfg, err);
    if (st != ATLAS_OK) {
        atlas_sem_config_free(&cfg);
        return st;
    }

    atlas_sem_input *inputs = calloc(ATLAS_SEM_DISCOVERY_MAX_CANDIDATES, sizeof(*inputs));
    size_t count = 0;
    if (inputs != NULL &&
        atlas_db_sem_inputs_get(db, repo->id, inputs, ATLAS_SEM_DISCOVERY_MAX_CANDIDATES, &count,
                                err) == ATLAS_OK) {
        /* `known` is "a walk has happened", not "the query succeeded". An empty
         * candidate list from a repository nobody has walked is Atlas not having
         * looked, and comparing it against a generation's recorded count would
         * report a change nobody made — the same rule as an empty stored
         * identity never making a generation stale. */
        out->inputs.known = cfg.discovery_state != ATLAS_SEM_DISC_UNKNOWN;
        out->inputs.discovery = cfg.discovery_state;
        for (size_t i = 0; i < count; i++) {
            if (!inputs[i].accepted) {
                out->inputs_rejected++;
            }
        }
    } else {
        count = 0;
    }
    out->auto_intent = cfg.auto_intent;
    {
        atlas_buf ident = ATLAS_BUF_INIT;
        atlas_err iderr;
        atlas_err_init(&iderr);
        if (atlas_db_repo_identity_hash(db, repo->id, &ident, &iderr) == ATLAS_OK) {
            (void)snprintf(out->repo_identity, sizeof out->repo_identity, "%s",
                           atlas_buf_cstr(&ident));
        }
        atlas_buf_free(&ident);
    }

    int root_fd = open(atlas_buf_cstr(&repo->root_path),
                       O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);

    /* The discovery digest, built as the accepted set is walked. Domain, state
     * and exclusions first, in the order `ATLAS_SEM_DISCOVERY_DOMAIN` documents
     * them — the two must agree, because a generation sealed by one is compared
     * against a value produced by the other. */
    atlas_sha256 dh;
    atlas_sha256_init(&dh);
    feed_str(&dh, ATLAS_SEM_DISCOVERY_DOMAIN);
    feed_str(&dh, atlas_sem_discovery_name(cfg.discovery_state));
    feed_str(&dh, cfg.excludes.len > 0 ? atlas_buf_cstr(&cfg.excludes) : "");

    char per_file[ATLAS_SEM_MAX_COMPDBS][65];
    size_t accepted = 0;
    for (size_t i = 0; i < count; i++) {
        if (!inputs[i].accepted) {
            continue;
        }
        out->inputs.accepted_count++;
        char digest[65];
        digest[0] = '\0';
        if (root_fd >= 0) {
            atlas_buf data = ATLAS_BUF_INIT;
            atlas_err ignored;
            atlas_err_init(&ignored);
            if (read_bounded(root_fd, inputs[i].path, ATLAS_CODE_MAX_COMPILE_DB_BYTES, &data,
                             &ignored) == ATLAS_OK) {
                atlas_sha256_hex(data.data, data.len, digest);
            }
            atlas_buf_free(&data);
        }
        /* A fixed marker rather than nothing, and it goes into **both** digests.
         *
         * Skipping an unreadable input would make a repository whose second
         * compilation database has just vanished compare equal to one that only
         * ever had the first — the same mistake A9.2.3 refuses when a file's
         * content hash is missing, and the more dangerous half of the trade. A
         * transient read failure costs one unnecessary rebuild, which is
         * self-correcting; an input that disappeared without moving the digest
         * leaves the index describing build descriptions that are gone, with
         * nothing saying so. */
        const char *contribution = digest[0] != '\0' ? digest : "atlas.sem.input.unreadable";
        feed_str(&dh, inputs[i].path);
        feed_str(&dh, contribution);
        if (accepted < (size_t)ATLAS_SEM_MAX_COMPDBS) {
            (void)snprintf(per_file[accepted], sizeof per_file[accepted], "%s", contribution);
            accepted++;
        }
    }
    if (root_fd >= 0) {
        (void)close(root_fd);
    }

    unsigned char raw[ATLAS_SHA256_DIGEST_LEN];
    atlas_sha256_final(&dh, raw);
    atlas_hex_encode(raw, sizeof raw, out->discovery_digest);
    out->discovery_digest[ATLAS_SHA256_HEX_LEN] = '\0';

    /* The narrower digest, over the same per-file hashes: it is what gives the
     * specific `a compilation database changed` reason, which the broader
     * identity cannot, because the identity also moves for an ordinary source
     * edit. Empty when nothing was accepted, and an empty live digest never
     * makes a generation stale.
     *
     * A **partly** readable set reports as changed, which reverses what A9.2.3's
     * `atlas_sem_live_compdb_digest` did: that function returned an empty digest
     * for the whole set the moment one file could not be read, on the argument
     * that "Atlas could not look" is not evidence of change. The argument holds
     * for a set of *named* paths, where an unreadable one is usually a typo. It
     * does not hold for a set Atlas discovered and accepted, where an unreadable
     * one usually means the build tree was removed — and that function is gone,
     * because it had no callers left once every live value came from one pass. */
    if (accepted > 0) {
        digest_list((const char (*)[65])per_file, accepted, out->compdb_digest);
    }

    /* And the source identity, from the discovery digest already in hand rather
     * than from a second read of the same files. */
    char content[65];
    content[0] = '\0';
    if (atlas_db_sem_source_content_digest(db, repo->id, content, err) == ATLAS_OK) {
        source_identity_from(out->discovery_digest, content, out->source_identity);
    }

    free(inputs);
    atlas_sem_config_free(&cfg);
    return ATLAS_OK;
}

/* --- applying one unit's facts ---------------------------------------------- */

typedef struct apply_ctx {
    atlas_db *db;
    int64_t generation_id;
    int64_t unit_id;
    int64_t pending;
    int64_t symbols;
    int64_t edges;
    int64_t includes;
    atlas_status st;
} apply_ctx;

/* Batched, so no write transaction is held across unbounded work — A1's rule.
 * A failure rolls the current batch back whole and fails the generation; it
 * never leaves half a unit applied and reported as complete. */
static atlas_status batch_checkpoint(apply_ctx *ctx, atlas_err *err) {
    if (ctx->pending < ATLAS_SEM_APPLY_BATCH) {
        return ATLAS_OK;
    }
    atlas_status st = atlas_db_commit(ctx->db, err);
    if (st != ATLAS_OK) {
        return st;
    }
    ctx->pending = 0;
    return atlas_db_begin(ctx->db, err);
}

static atlas_status on_fact(const atlas_sem_fact *fact, void *ud, atlas_err *err) {
    apply_ctx *ctx = (apply_ctx *)ud;
    atlas_status st = ATLAS_OK;
    if (strcmp(fact->record, "symbol") == 0) {
        st = atlas_db_sem_symbol_add(ctx->db, ctx->generation_id, fact, err);
        ctx->symbols++;
    } else if (strcmp(fact->record, "edge") == 0) {
        st = atlas_db_sem_edge_add(ctx->db, ctx->generation_id, ctx->unit_id, fact, err);
        ctx->edges++;
    } else if (strcmp(fact->record, "include") == 0) {
        st = atlas_db_sem_include_add(ctx->db, ctx->generation_id, fact, err);
        ctx->includes++;
    }
    if (st != ATLAS_OK) {
        return st;
    }
    ctx->pending++;
    return batch_checkpoint(ctx, err);
}

/* --- the input digest -------------------------------------------------------
 *
 * What makes incremental indexing correct rather than merely fast.
 *
 * It covers the unit's own source bytes, the bytes of every file the *previous*
 * generation recorded it as including, and its configuration digest. The
 * previous generation's include list is the honest source for "what does this
 * unit depend on": Atlas cannot know a unit's includes without preprocessing
 * it, and preprocessing it is the work being avoided. A unit Atlas has never
 * parsed has no include list and is therefore always parsed, which is the safe
 * direction.
 *
 * This is why editing a shared header invalidates every unit that includes it:
 * the header's content hash is in each of their digests. */
typedef struct hash_grab {
    atlas_buf *out;
    bool got;
} hash_grab;

/* Row callbacks hand out borrowed pointers valid for the call only, so the hash
 * is copied rather than aliased — the memory-ownership rule, and the one that
 * has bitten this codebase before. */
static atlas_status take_hash(const atlas_file_row *row, void *ud, atlas_err *err) {
    hash_grab *g = (hash_grab *)ud;
    if (row->content_hash == NULL || row->content_hash[0] == '\0') {
        return ATLAS_OK;
    }
    atlas_status st = atlas_buf_set_str(g->out, row->content_hash, err);
    if (st == ATLAS_OK) {
        g->got = true;
    }
    return st;
}

static atlas_status file_content_hash(atlas_db *db, int64_t repo_id, const char *rel, size_t len,
                                      atlas_buf *out, bool *found, atlas_err *err) {
    hash_grab g = {out, false};
    bool present = false;
    atlas_status st = atlas_db_file_get(db, repo_id, rel, len, take_hash, &g, &present, err);
    *found = st == ATLAS_OK && present && g.got;
    return st;
}

static atlas_status input_digest(atlas_db *db, int64_t repo_id, int64_t prev_gen,
                                 const char *source_rel, const char *config_digest, char out[65],
                                 atlas_err *err) {
    atlas_sha256 h;
    atlas_sha256_init(&h);
    /* v2 since A9.2.4: the **producer** is part of a unit's inputs.
     *
     * It was not, and that made the analyzer epoch unenforceable. Bumping
     * `ATLAS_SEM_ANALYZER_VERSION` makes a generation *stale*, which schedules a
     * rebuild — and the rebuild then compared a digest that knew nothing about
     * the analyzer, found every unit unchanged, and carried forward facts the
     * *old* analyzer had produced. The new generation recorded the new version
     * and contained the old graph.
     *
     * A3 states the rule the epoch exists for: bump it whenever a pass would
     * produce different facts from identical bytes, and the next pass rebuilds.
     * The second half was not true of an incremental pass, so every analyzer
     * bump in Atlas' history was a no-op unless somebody happened to pass
     * `--rebuild`. Measured on this machine: after bumping to 2, a scheduled
     * rebuild reused 203 of 203 units and republished the same graph under the
     * new version.
     *
     * The compiler version is here for the same reason and with the same force:
     * `atlas_sem_freshness_of` already treats a changed compiler as stale, and
     * that staleness was equally unenforceable. */
    static const char DOMAIN[] = "atlas.sem.unit.v2";
    atlas_sha256_update(&h, DOMAIN, sizeof(DOMAIN));
    atlas_sha256_update(&h, ATLAS_SEM_ANALYZER_ID, sizeof(ATLAS_SEM_ANALYZER_ID));
    char av[32];
    int avn = snprintf(av, sizeof av, "%d", ATLAS_SEM_ANALYZER_VERSION);
    if (avn > 0) {
        atlas_sha256_update(&h, av, (size_t)avn);
    }
    const char *cv = atlas_sem_compiler_version();
    atlas_sha256_update(&h, cv, strlen(cv));
    atlas_sha256_update(&h, config_digest, strlen(config_digest));

    atlas_buf paths = ATLAS_BUF_INIT;
    atlas_status st = atlas_buf_append(&paths, source_rel, strlen(source_rel) + 1, err);
    if (st == ATLAS_OK && prev_gen > 0) {
        st = atlas_db_sem_unit_inputs(db, prev_gen, source_rel, &paths, err);
    }
    if (st != ATLAS_OK) {
        atlas_buf_free(&paths);
        return st;
    }

    /* Each path's content hash comes from the *file index*, which the
     * reconciliation pass already maintains. Nothing is read from disk here:
     * asking the index is both cheaper and consistent with the generation's own
     * claim to describe one indexed state. A path the index does not hold
     * contributes its name and an explicit absence, so a deleted header changes
     * the digest rather than silently leaving it alone. */
    const char *p = (const char *)paths.data;
    const char *end = p + paths.len;
    while (p < end && st == ATLAS_OK) {
        size_t n = strlen(p);
        atlas_buf hash = ATLAS_BUF_INIT;
        bool found = false;
        st = file_content_hash(db, repo_id, p, n, &hash, &found, err);
        if (st == ATLAS_OK) {
            uint64_t ln = (uint64_t)n;
            unsigned char lb[8];
            for (int b = 0; b < 8; b++) {
                lb[b] = (unsigned char)((ln >> (8 * (7 - b))) & 0xffu);
            }
            atlas_sha256_update(&h, lb, sizeof(lb));
            atlas_sha256_update(&h, p, n);
            if (found && hash.len > 0) {
                atlas_sha256_update(&h, hash.data, hash.len);
            } else {
                /* A9.2.5, found by ASan. This read one byte past its own string
                 * literal, every time, since the marker was introduced.
                 *
                 * `"\x00absent"` does not mean `NUL` followed by `absent`. A C
                 * hex escape consumes *every* following hex digit, and `0`, `0`,
                 * `a` and `b` are all hex digits — so the literal compiles to
                 * `{0xAB,'s','e','n','t',0}`, six bytes, and the length below
                 * said seven. The overflowing byte is whatever `.rodata` holds
                 * next, which is undefined behaviour and, worse for this
                 * function, is **not stable across builds**: a unit whose
                 * include closure holds a file the index cannot vouch for could
                 * therefore digest differently after a relink and be reparsed
                 * for no reason.
                 *
                 * Written as two adjacent literals, which is what stops the
                 * escape: string concatenation happens after escapes are
                 * resolved, so `"\x00" "absent"` is exactly the seven bytes
                 * intended. `sizeof - 1` rather than a written 7, so the length
                 * can never disagree with the bytes again. */
                static const char ABSENT_MARKER[] = "\x00" "absent";
                atlas_sha256_update(&h, ABSENT_MARKER, sizeof ABSENT_MARKER - 1u);
            }
        }
        atlas_buf_free(&hash);
        p += n + 1;
    }
    atlas_buf_free(&paths);
    if (st != ATLAS_OK) {
        return st;
    }

    unsigned char digest[ATLAS_SHA256_DIGEST_LEN];
    atlas_sha256_final(&h, digest);
    atlas_hex_encode(digest, sizeof(digest), out);
    out[ATLAS_SHA256_HEX_LEN] = '\0';
    return ATLAS_OK;
}

/* Seals one unit's input digest against the finished generation. */
typedef struct seal_ctx {
    atlas_db *db;
    int64_t repo_id;
    int64_t generation_id;
    atlas_status st;
} seal_ctx;

static atlas_status seal_unit(const atlas_sem_unit_key *key, void *ud, atlas_err *err) {
    seal_ctx *c = (seal_ctx *)ud;
    char digest[65];
    /* Copied first: the key's pointers are borrowed from a live statement, and
     * `input_digest` runs its own queries on the same connection. */
    char source[4096];
    char config[65];
    (void)snprintf(source, sizeof source, "%s", key->source_text);
    (void)snprintf(config, sizeof config, "%s", key->config_digest);

    c->st = input_digest(c->db, c->repo_id, c->generation_id, source, config, digest, err);
    if (c->st != ATLAS_OK) {
        return c->st;
    }
    c->st = atlas_db_sem_unit_set_digest(c->db, c->generation_id, source, config, digest, err);
    return c->st;
}

/* --- the pass ---------------------------------------------------------------- */

atlas_status atlas_sem_index_run(atlas_db *db, int64_t repo_id, const atlas_sem_index_opts *opts,
                                 atlas_sem_index_summary *sum, atlas_err *err) {
    if (db == NULL || opts == NULL || sum == NULL || opts->root == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "semantic index: bad request");
    }
    atlas_sem_index_summary_init(sum);

    if (!atlas_sem_available()) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG,
                             "this Atlas was built without libclang, so it cannot build a "
                             "compiler-derived semantic index");
    }
    if (opts->compdbs == NULL || opts->compdbs_len == 0) {
        /* No search, ever. A pass with nothing to read says so. */
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "no compilation database was named; Atlas does not search a "
                             "repository for one");
    }

    int64_t started = now_ms();

    /* --- 1. the compilation databases --- */
    compdb_slot slots[ATLAS_SEM_MAX_COMPDBS];
    size_t slot_count = 0;
    memset(slots, 0, sizeof(slots));

    atlas_status st = ATLAS_OK;
    const char *p = opts->compdbs;
    const char *end = p + opts->compdbs_len;
    while (p < end && *p != '\0' && st == ATLAS_OK) {
        if (slot_count >= ATLAS_SEM_MAX_COMPDBS) {
            st = atlas_err_set(err, ATLAS_ERR_USAGE,
                               "more than %d compilation databases were named",
                               ATLAS_SEM_MAX_COMPDBS);
            break;
        }
        st = load_compdb(opts->root_fd,
                         opts->origin_root != NULL ? opts->origin_root : opts->root, p,
                         &slots[slot_count], err);
        if (st == ATLAS_OK) {
            slot_count++;
        }
        p += strlen(p) + 1;
    }
    if (st != ATLAS_OK) {
        for (size_t i = 0; i < slot_count; i++) {
            slot_free(&slots[i]);
        }
        return st;
    }

    /* A9.2.3: a build description that names no translation unit is refused,
     * exactly as a missing one is — and this is not a tidiness check.
     *
     * `atlas_code_compdb_parse` reduces every entry through a positive allowlist
     * and drops what it cannot use, so a truncated, malformed or half-written
     * `compile_commands.json` yields *zero units* rather than an error. Before
     * this refusal the pass then built a generation describing nothing, published
     * it, and replaced a perfectly good one: on the installed acceptance
     * repository a corrupted database turned `4 of 4 source files` into `0 of 4`
     * in one automatic rebuild.
     *
     * The coverage model held — `scope_covered = 0` cannot support any absence,
     * so nothing false was ever concluded — but the operational guarantee did
     * not, and "a failed rebuild preserves the last-known-good generation" is the
     * guarantee. Failing here is what makes it true: no generation is opened, the
     * previous one stays current, and the daemon's governor records the attempt
     * so it does not retry until the description changes.
     *
     * A repository whose build genuinely compiles nothing has nothing to index,
     * and saying so is better than publishing an index of nothing. */
    size_t declared_units = 0;
    for (size_t i = 0; i < slot_count; i++) {
        declared_units += slots[i].parsed.unit_count;
    }
    if (declared_units == 0) {
        for (size_t i = 0; i < slot_count; i++) {
            slot_free(&slots[i]);
        }
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "the named compilation databases describe no translation unit that "
                             "Atlas can read; the previous semantic index is unchanged");
    }

    char all_digest[65];
    digest_all(slots, slot_count, all_digest);

    /* --- 2. is there anything to do? --- */
    atlas_sem_generation prev;
    bool have_prev = false;
    st = atlas_db_sem_current(db, repo_id, &prev, &have_prev, err);
    if (st != ATLAS_OK) {
        goto cleanup;
    }

    /* Is there anything to do?
     *
     * The answer is **every unit's input digest**, not the commit id. An
     * earlier version of this compared the commit, the compilation databases,
     * the compiler and the analyzer, and short-circuited when all four matched
     * — and that is wrong in the ordinary case: Atlas indexes the working tree,
     * so a file can change with the commit standing still, and a repository
     * with uncommitted edits would have been reported unchanged for ever.
     * `tests/test_sem.c` fails on exactly that.
     *
     * So the check is the same digest comparison the incremental path uses, run
     * over every unit before any generation exists. It reads rows and hashes
     * nothing that was not already hashed by the file index — comparing digests
     * is not rebuilding — and when it finds everything unchanged it returns
     * *without creating a generation at all*, which is both cheaper than
     * copying every row forward and more honest: the state Atlas is serving
     * genuinely is the state that was measured. */
    bool prereqs_match =
        !opts->rebuild && have_prev && prev.status == ATLAS_SEM_GEN_COMPLETE &&
        strcmp(prev.compdb_digest, all_digest) == 0 &&
        strcmp(prev.compiler_version, atlas_sem_compiler_version()) == 0 &&
        strcmp(prev.analyzer_id, ATLAS_SEM_ANALYZER_ID) == 0 &&
        prev.analyzer_version == ATLAS_SEM_ANALYZER_VERSION;

    if (prereqs_match) {
        bool all_same = true;
        int64_t seen = 0;
        for (size_t si = 0; all_same && si < slot_count; si++) {
            const atlas_code_compdb *cdb = &slots[si].parsed;
            for (size_t ui = 0; ui < cdb->unit_count; ui++) {
                const atlas_code_cu *cu = &cdb->units[ui];
                const char *source_rel = atlas_code_compdb_str(cdb, cu->source_off);
                char config[65];
                st = atlas_sem_config_digest(cdb, ui, config, err);
                if (st != ATLAS_OK) {
                    goto cleanup;
                }
                char want[65];
                st = input_digest(db, repo_id, prev.id, source_rel, config, want, err);
                if (st != ATLAS_OK) {
                    goto cleanup;
                }
                atlas_buf had = ATLAS_BUF_INIT;
                bool found = false;
                st = atlas_db_sem_unit_digest(db, prev.id, source_rel, config, &had, &found, err);
                bool same = st == ATLAS_OK && found && strcmp(atlas_buf_cstr(&had), want) == 0;
                atlas_buf_free(&had);
                if (st != ATLAS_OK) {
                    goto cleanup;
                }
                seen++;
                if (!same) {
                    all_same = false;
                    break;
                }
            }
        }
        /* A9.2.3: the *scope* must be unchanged too, not only the units.
         *
         * A repository can gain a `.c` file the compilation database does not
         * name. Every unit's input digest is then identical, so the unit
         * comparison above finds nothing — and yet what Atlas may claim has
         * changed, because the tree now holds a source this index did not read.
         * Short-circuiting on the units alone would carry the previous
         * generation's manifest forward unchanged and keep reporting a
         * `scope_uncovered` measured against a tree that has since grown, which
         * is precisely the overclaim the manifest exists to end.
         *
         * When the scope has moved this is not a no-change run. The generation
         * that follows reuses every unit — so it costs a copy rather than a
         * reparse — and seals a manifest measured against the tree as it is. */
        if (all_same && seen == prev.tu_total) {
            int64_t cand = 0;
            int64_t cov = 0;
            st = atlas_db_sem_scope_counts(db, repo_id, prev.id, &cand, &cov, err);
            if (st != ATLAS_OK) {
                goto cleanup;
            }
            atlas_index_state fs;
            atlas_index_state_init(&fs);
            atlas_status fst = atlas_db_index_state_get(db, repo_id, &fs, err);
            bool file_current = fst == ATLAS_OK && atlas_index_state_is_current(&fs, NULL);
            atlas_index_state_free(&fs);
            if (fst != ATLAS_OK) {
                st = fst;
                goto cleanup;
            }
            atlas_sem_scope_discovery disc =
                file_current ? ATLAS_SEM_SCOPE_DECLARED : ATLAS_SEM_SCOPE_UNKNOWN;
            if (cand != prev.scope_candidates || cov != prev.scope_covered ||
                disc != prev.scope_discovery) {
                all_same = false;
            }
        }
        if (all_same && seen == prev.tu_total) {
            /* A9.2.3: re-stamp the source identity, and this is not optional.
             *
             * A repository can hold a source the compilation database does not
             * name — on a real tree, hundreds of them. Editing one moves the
             * live source identity, because that identity covers every source
             * and header the *file index* holds; it moves no unit digest and no
             * scope count, because the file was never compiled and was already
             * counted as uncovered. So freshness reads STALE, the scheduler
             * queues a build, the build finds nothing to do and publishes
             * nothing — and the stored identity stays old. The repository is
             * stale again on the next tick and rebuilds every sweep, for ever,
             * achieving nothing.
             *
             * Re-stamping is honest rather than a paper over. This pass has just
             * verified that every input which determines what the generation
             * would contain is identical: each unit's digest over its transitive
             * include closure, the compilation-database digest, the compiler,
             * the analyzer, and the scope counts. The generation therefore
             * describes the new tree to exactly the same extent it described the
             * old one, and saying so is a statement Atlas can support. It is the
             * same move as sealing a unit's input digest at the end of a pass:
             * recording what was measured, once the measurement is complete.
             *
             * A failure here is not fatal to the answer — the previous
             * generation is still correct and still served — but it is worth
             * reporting, because a repository that cannot record its identity is
             * one that will rebuild on every sweep. */
            char ident[65];
            ident[0] = '\0';
            atlas_repo_info ri;
            atlas_repo_info_init(&ri);
            ri.id = repo_id;
            st = atlas_buf_set_str(&ri.root_path, opts->root, err);
            if (st == ATLAS_OK) {
                st = atlas_sem_source_identity(db, &ri, ident, err);
            }
            atlas_repo_info_free(&ri);
            if (st == ATLAS_OK && strcmp(ident, prev.source_identity) != 0) {
                /* Its own small transaction: nothing else is being written, and
                 * the identity is measured before it opens — A1's rule that no
                 * file read happens inside a write transaction. */
                st = atlas_db_begin(db, err);
                if (st == ATLAS_OK) {
                    st = atlas_db_sem_source_identity_set(db, prev.id, ident, err);
                }
                if (st == ATLAS_OK) {
                    st = atlas_db_commit(db, err);
                } else {
                    atlas_db_rollback(db);
                }
            }
            if (st != ATLAS_OK) {
                goto cleanup;
            }

            sum->no_change = true;
            sum->published = true;
            sum->generation_id = prev.id;
            sum->units_total = prev.tu_total;
            sum->units_reused = prev.tu_total;
            sum->units_complete = prev.tu_complete;
            sum->symbols = prev.symbol_count;
            sum->edges = prev.edge_count;
            sum->includes = prev.include_count;
            sum->duration_ms = now_ms() - started;
            goto cleanup;
        }
    }

    /* --- 3. open a generation --- */

    /* The first yield point, and the reason it is here rather than only in the
     * loop below: reading and parsing every compilation database in a repository
     * is already work, and until this moment the pass has done all of it holding
     * the thread. Offered before the transaction that opens the generation and
     * writes the compilation-database rows, so nothing is open when it is made. */
    if (opts->yield != NULL) {
        opts->yield(opts->yield_ud);
    }

    atlas_sem_generation gen;
    atlas_sem_generation_init(&gen);
    gen.repo_id = repo_id;
    (void)snprintf(gen.repo_identity_hash, sizeof(gen.repo_identity_hash), "%s",
                   opts->repo_identity_hash != NULL ? opts->repo_identity_hash : "");
    (void)snprintf(gen.commit_id, sizeof(gen.commit_id), "%s",
                   opts->commit_id != NULL ? opts->commit_id : "");
    (void)snprintf(gen.compdb_digest, sizeof(gen.compdb_digest), "%s", all_digest);
    gen.compdb_count = (int64_t)slot_count;
    (void)snprintf(gen.compiler_id, sizeof(gen.compiler_id), "%s", atlas_sem_compiler_id());
    (void)snprintf(gen.compiler_version, sizeof(gen.compiler_version), "%s",
                   atlas_sem_compiler_version());
    (void)snprintf(gen.analyzer_id, sizeof(gen.analyzer_id), "%s", ATLAS_SEM_ANALYZER_ID);
    gen.analyzer_version = ATLAS_SEM_ANALYZER_VERSION;

    st = atlas_db_begin(db, err);
    if (st != ATLAS_OK) {
        goto cleanup;
    }
    st = atlas_db_sem_generation_begin(db, &gen, &sum->generation_id, err);
    for (size_t i = 0; st == ATLAS_OK && i < slot_count; i++) {
        st = atlas_db_sem_compdb_add(db, sum->generation_id, atlas_buf_cstr(&slots[i].rel),
                                     slots[i].digest, slots[i].parsed.entries_seen,
                                     slots[i].parsed.entries_dropped, &slots[i].row_id, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_rollback(db);
        goto cleanup;
    }
    st = atlas_db_commit(db, err);
    if (st != ATLAS_OK) {
        goto cleanup;
    }

    int64_t prev_gen = have_prev && !opts->rebuild ? prev.id : 0;
    int64_t max_units = opts->max_units > 0 ? opts->max_units : ATLAS_SEM_MAX_UNITS;

    /* --- 4. every unit, in a deterministic order ---
     *
     * Databases in the order named, entries in the order the document gave
     * them. Reproducible, so two runs over one state produce identical
     * generations and the context builder's ranking can be deterministic. */
    for (size_t si = 0; st == ATLAS_OK && si < slot_count; si++) {
        const atlas_code_compdb *cdb = &slots[si].parsed;
        for (size_t ui = 0; ui < cdb->unit_count; ui++) {
            if (sum->units_total >= max_units) {
                sum->truncated = true;
                sum->truncated_reason = "the per-generation translation-unit ceiling was reached";
                break;
            }
            if (opts->cancel != NULL && opts->cancel(opts->cancel_ud)) {
                st = atlas_err_set(err, ATLAS_ERR_USAGE, "the semantic index was cancelled");
                break;
            }
            /* The yield the season exists for, beside the cancel poll because
             * this is the one place in the pass where nothing is open: the
             * previous unit's transaction has committed and the next one has not
             * begun. Asked between units and never inside one — a unit's
             * transaction deliberately spans its parse child, and lending the
             * thread out from inside it would put a second write in the middle
             * of a transaction this pass is holding. */
            if (opts->yield != NULL) {
                opts->yield(opts->yield_ud);
            }
            sum->units_total++;

            const atlas_code_cu *cu = &cdb->units[ui];
            const char *source_rel = atlas_code_compdb_str(cdb, cu->source_off);
            char config[65];
            st = atlas_sem_config_digest(cdb, ui, config, err);
            if (st != ATLAS_OK) {
                break;
            }

            char want[65];
            st = input_digest(db, repo_id, prev_gen, source_rel, config, want, err);
            if (st != ATLAS_OK) {
                break;
            }

            /* Carry forward when the inputs are byte-identical. */
            bool reused = false;
            if (prev_gen > 0) {
                atlas_buf had = ATLAS_BUF_INIT;
                bool found = false;
                st = atlas_db_sem_unit_digest(db, prev_gen, source_rel, config, &had, &found, err);
                if (st == ATLAS_OK && found && strcmp(atlas_buf_cstr(&had), want) == 0) {
                    st = atlas_db_begin(db, err);
                    int64_t syms = 0, edges = 0;
                    /* **The unit row first, then its facts.**
                     *
                     * The order is the fix for A9.2.4's worst find. An edge
                     * belongs to the unit that produced it, and a carried-forward
                     * edge must belong to the unit row in *this* generation — so
                     * that row has to exist before the copy can name it. The
                     * previous order made that impossible, so the copy carried
                     * the ancestor generation's `unit_id` across instead, and the
                     * graph decayed on every incremental pass: the next pass
                     * selects the edges to carry by joining `sem_units`, and once
                     * the ancestor's rows were pruned the join found nothing.
                     * Measured on a real repository, 475,741 edges became 10,631
                     * over four passes.
                     *
                     * Written twice on purpose: once to exist, once to record
                     * what it carried. `atlas_db_sem_unit_add` upserts on the
                     * table's unique key, so the second write updates rather than
                     * duplicating, and both are inside the one transaction. */
                    int64_t unit_id = 0;
                    atlas_sem_unit_row row;
                    memset(&row, 0, sizeof(row));
                    row.generation_id = sum->generation_id;
                    row.source_text = source_rel;
                    row.compdb_id = slots[si].row_id;
                    row.config_digest = config;
                    row.input_digest = want;
                    row.status = ATLAS_SEM_TU_COMPLETE;
                    row.reused = true;
                    if (st == ATLAS_OK) {
                        st = atlas_db_sem_unit_add(db, &row, &unit_id, err);
                    }
                    if (st == ATLAS_OK) {
                        st = atlas_db_sem_copy_unit(db, prev_gen, sum->generation_id, unit_id,
                                                    source_rel, config, &syms, &edges, err);
                    }
                    if (st == ATLAS_OK) {
                        row.symbols = syms;
                        row.edges = edges;
                        st = atlas_db_sem_unit_add(db, &row, NULL, err);
                    }
                    if (st == ATLAS_OK) {
                        st = atlas_db_commit(db, err);
                    } else {
                        atlas_db_rollback(db);
                    }
                    if (st == ATLAS_OK) {
                        reused = true;
                        sum->units_reused++;
                        sum->units_complete++;
                        sum->symbols += syms;
                        sum->edges += edges;
                    }
                }
                atlas_buf_free(&had);
            }
            if (st != ATLAS_OK || reused) {
                continue;
            }

            /* --- parse, outside any transaction --- */
            atlas_buf abs = ATLAS_BUF_INIT;
            st = atlas_buf_appendf(&abs, err, "%s/%s", opts->root, source_rel);
            if (st != ATLAS_OK) {
                break;
            }

            /* The argument vector, built from the *validated* record. Include
             * directories, defines and undefines, the standard and the explicit
             * language, and nothing else — the compile-database reader already
             * refused everything not on that list, and the child refuses again
             * at the point of use. */
            const char *args[ATLAS_CODE_MAX_INCLUDE_DIRS_PER_UNIT * 2 +
                             ATLAS_CODE_MAX_DEFINES_PER_UNIT + 8];
            size_t argc = 0;
            atlas_buf argbuf = ATLAS_BUF_INIT;
            size_t offsets[sizeof(args) / sizeof(args[0])];
            size_t noff = 0;

            for (size_t k = 0; k < cu->incdir_count && argc < sizeof(args) / sizeof(args[0]) - 4;
                 k++) {
                const atlas_code_cu_incdir *d = &cdb->incdirs[cu->incdir_first + k];
                const char *flag = "-I";
                switch ((atlas_code_incdir_kind)d->kind) {
                    case ATLAS_CODE_INCDIR_QUOTE:
                        flag = "-iquote";
                        break;
                    case ATLAS_CODE_INCDIR_SYSTEM:
                        flag = "-isystem";
                        break;
                    case ATLAS_CODE_INCDIR_AFTER:
                        flag = "-idirafter";
                        break;
                    case ATLAS_CODE_INCDIR_SEARCH:
                    default:
                        flag = "-I";
                        break;
                }
                const char *dir = atlas_code_compdb_str(cdb, d->path_off);
                offsets[noff++] = argbuf.len;
                st = atlas_buf_append(&argbuf, flag, strlen(flag) + 1, err);
                if (st != ATLAS_OK) {
                    break;
                }
                offsets[noff++] = argbuf.len;
                /* An internal directory is repository-relative and is made
                 * absolute against the root Atlas resolved. An external one is
                 * recorded and passed as given, and is never opened by Atlas
                 * itself — the compiler may read a system header, which is what
                 * `-isystem` means. */
                if (d->external) {
                    st = atlas_buf_append(&argbuf, dir, strlen(dir) + 1, err);
                } else {
                    st = atlas_buf_appendf(&argbuf, err, "%s/%s", opts->root, dir);
                    if (st == ATLAS_OK) {
                        st = atlas_buf_append(&argbuf, "", 1, err);
                    }
                }
                if (st != ATLAS_OK) {
                    break;
                }
            }
            for (size_t k = 0;
                 st == ATLAS_OK && k < cu->define_count && noff < sizeof(offsets) / sizeof(offsets[0]) - 4;
                 k++) {
                const atlas_code_cu_define *d = &cdb->defines[cu->define_first + k];
                const char *name = atlas_code_compdb_str(cdb, d->name_off);
                offsets[noff++] = argbuf.len;
                if (d->undef) {
                    st = atlas_buf_appendf(&argbuf, err, "-U%s", name);
                } else if (d->value_len > 0) {
                    st = atlas_buf_appendf(&argbuf, err, "-D%s=%s", name,
                                           atlas_code_compdb_str(cdb, d->value_off));
                } else {
                    st = atlas_buf_appendf(&argbuf, err, "-D%s", name);
                }
                if (st == ATLAS_OK) {
                    st = atlas_buf_append(&argbuf, "", 1, err);
                }
            }
            if (st == ATLAS_OK && cu->std_len > 0) {
                offsets[noff++] = argbuf.len;
                st = atlas_buf_appendf(&argbuf, err, "-std=%s",
                                       atlas_code_compdb_str(cdb, cu->std_off));
                if (st == ATLAS_OK) {
                    st = atlas_buf_append(&argbuf, "", 1, err);
                }
            }
            if (st == ATLAS_OK && cu->lang_len > 0) {
                offsets[noff++] = argbuf.len;
                st = atlas_buf_append(&argbuf, "-x", 3, err);
                if (st == ATLAS_OK) {
                    offsets[noff++] = argbuf.len;
                    const char *lang = atlas_code_compdb_str(cdb, cu->lang_off);
                    st = atlas_buf_append(&argbuf, lang, strlen(lang) + 1, err);
                }
            }
            if (st != ATLAS_OK) {
                atlas_buf_free(&abs);
                atlas_buf_free(&argbuf);
                break;
            }
            for (size_t k = 0; k < noff; k++) {
                args[argc++] = (const char *)argbuf.data + offsets[k];
            }

            atlas_sem_parse_req req;
            memset(&req, 0, sizeof(req));
            req.source = atlas_buf_cstr(&abs);
            req.root = opts->root;
            req.directory = atlas_code_compdb_str(cdb, cu->dir_off);
            req.args = args;
            req.arg_count = argc;

            apply_ctx ctx;
            memset(&ctx, 0, sizeof(ctx));
            ctx.db = db;
            ctx.generation_id = sum->generation_id;

            /* The unit row is written first so the edges can name it, and the
             * whole unit is one transaction: a unit is applied entirely or not
             * at all. */
            atlas_sem_unit_row row;
            memset(&row, 0, sizeof(row));
            row.generation_id = sum->generation_id;
            row.source_text = source_rel;
            row.compdb_id = slots[si].row_id;
            row.config_digest = config;
            row.input_digest = want;
            row.status = ATLAS_SEM_TU_FAILED;

            st = atlas_db_begin(db, err);
            if (st == ATLAS_OK) {
                st = atlas_db_sem_unit_add(db, &row, &ctx.unit_id, err);
            }
            if (st != ATLAS_OK) {
                atlas_db_rollback(db);
                atlas_buf_free(&abs);
                atlas_buf_free(&argbuf);
                break;
            }

            atlas_sem_parse_result pres;
            atlas_status pst =
                atlas_sem_parse_unit(opts->atlas_exe, &req, on_fact, &ctx, &pres, err);

            /* A9.2.5. A bounded second attempt, and only for a failure that a
             * second attempt could plausibly change.
             *
             * A parse child that was OOM-killed or that ran out of wall clock
             * failed for a reason that has nothing to do with the bytes — and
             * before this season the consequence was permanent: `tu_failed > 0`
             * makes coverage incomplete, the retry governor compares identities,
             * and identical bytes never retry. One transient memory-pressure
             * event therefore cost a repository the ability to state an absence
             * until somebody happened to edit a file.
             *
             * **Two compile-time bounds, and the second is not redundant.**
             * `ATLAS_SEM_UNIT_TRANSIENT_RETRIES` bounds one unit;
             * `ATLAS_SEM_PASS_TRANSIENT_RETRIES` bounds the pass, because a
             * per-unit bound alone still permits every unit to retry and a
             * `TIMEOUT` retry costs a whole parse timeout again — so the worst
             * case without it is twice the pass, inside the write transaction
             * each unit holds while its child runs.
             *
             * Nothing durable records that a retry happened, so a restart has no
             * half-finished state to interpret; nothing schedules a later
             * attempt, so no timer can spin. A unit that fails twice is recorded
             * failed exactly as it was before, and the generation is visibly
             * INCOMPLETE. */
            for (int attempt = 0;
                 pst == ATLAS_OK && ctx.st == ATLAS_OK &&
                 attempt < ATLAS_SEM_UNIT_TRANSIENT_RETRIES &&
                 sum->units_retried < ATLAS_SEM_PASS_TRANSIENT_RETRIES &&
                 pres.status == ATLAS_SEM_TU_FAILED && atlas_sem_why_is_transient(pres.why);
                 attempt++) {
                /* The counters are reset so the unit row records what the
                 * *successful* attempt produced rather than the sum of both.
                 *
                 * The rows a partial first attempt already wrote are not deleted,
                 * and do not need to be: every fact insert is
                 * `ON CONFLICT(...) DO NOTHING` on a natural key —
                 * `(generation_id, usr, file_text, line, is_definition)` for a
                 * symbol, `(generation_id, kind, src_usr, dst_usr, file_text,
                 * line, col)` for an edge — so re-inserting the same facts is a
                 * no-op. Both attempts parse the same bytes with the same
                 * arguments, so the first attempt's output is a prefix of the
                 * second's and the union is the second's set. Deleting them
                 * instead is not available: `sem_symbols` and `sem_includes` are
                 * generation-scoped, not unit-scoped, so there is no correct
                 * "this unit's rows" to remove.
                 *
                 * The generation's published counts come from
                 * `atlas_db_sem_generation_counts`, which reads the rows that
                 * exist rather than these counters, so nothing downstream can be
                 * inflated by a retry either. */
                ctx.symbols = 0;
                ctx.edges = 0;
                ctx.includes = 0;
                sum->units_retried++;
                pst = atlas_sem_parse_unit(opts->atlas_exe, &req, on_fact, &ctx, &pres, err);
            }

            if (pst != ATLAS_OK) {
                st = pst;
            } else if (ctx.st != ATLAS_OK) {
                st = ctx.st;
            }

            /* The input digest is *not* computed here. It is sealed for every
             * unit at the end of the pass, once the whole generation's include
             * rows exist — see the sealing step below for why. */
            if (st == ATLAS_OK) {
                row.status = pres.status;
                row.why = pres.why;
                row.diagnostics_errors = pres.diagnostics_errors;
                row.symbols = ctx.symbols;
                row.edges = ctx.edges;
                row.duration_ms = pres.duration_ms;
                st = atlas_db_sem_unit_add(db, &row, NULL, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_db_commit(db, err);
            } else {
                atlas_db_rollback(db);
            }

            atlas_buf_free(&abs);
            atlas_buf_free(&argbuf);
            if (st != ATLAS_OK) {
                break;
            }

            sum->units_parsed++;
            sum->symbols += ctx.symbols;
            sum->edges += ctx.edges;
            sum->includes += ctx.includes;
            switch (pres.status) {
                case ATLAS_SEM_TU_COMPLETE:
                    sum->units_complete++;
                    break;
                case ATLAS_SEM_TU_PARTIAL:
                    sum->units_partial++;
                    break;
                case ATLAS_SEM_TU_UNSUPPORTED:
                    sum->units_unsupported++;
                    break;
                case ATLAS_SEM_TU_FAILED:
                case ATLAS_SEM_TU_UNKNOWN:
                default:
                    sum->units_failed++;
                    break;
            }
            if (pres.truncated) {
                sum->truncated = true;
                sum->truncated_reason = "a per-unit fact ceiling was reached";
            }
        }
    }

    /* The last yield point: the unit loop has ended and the sealing transaction
     * has not opened. A pass whose final unit was the slow one would otherwise
     * run sealing, candidates and publication back to back without offering the
     * thread once more, and those are the seconds a caller waiting behind it
     * feels last. */
    if (opts->yield != NULL) {
        opts->yield(opts->yield_ud);
    }

    /* --- 4b. seal every unit's input digest ---
     *
     * Once, at the end, and this placement is the whole correctness argument.
     *
     * A unit's input digest covers the *transitive closure* of what it
     * includes, and that closure is assembled from include rows contributed by
     * every unit in the generation — a header's own `#include` lines are
     * recorded by whichever unit first preprocessed it. Computing a unit's
     * digest immediately after its own parse therefore measured whatever
     * happened to have been recorded by then, which depends on the order the
     * units were processed in.
     *
     * The consequence was not subtle: the next run computed a *different*
     * digest from byte-identical inputs, found a mismatch, and reparsed the
     * same units for ever. Incremental indexing never converged. On DNA it
     * reparsed 38 of 416 units on every run, indefinitely.
     *
     * Sealing here makes the digest a function of the finished generation
     * alone, which is what a later run recomputes and compares against. */
    if (st == ATLAS_OK) {
        st = atlas_db_begin(db, err);
        if (st == ATLAS_OK) {
            seal_ctx sc;
            memset(&sc, 0, sizeof(sc));
            sc.db = db;
            sc.repo_id = repo_id;
            sc.generation_id = sum->generation_id;
            st = atlas_db_sem_units_all(db, sum->generation_id, seal_unit, &sc, err);
            if (st == ATLAS_OK) {
                st = sc.st;
            }
        }
        if (st == ATLAS_OK) {
            st = atlas_db_commit(db, err);
        } else {
            atlas_db_rollback(db);
        }
    }

    /* --- 5. candidates, once, over the whole generation --- */
    if (st == ATLAS_OK) {
        st = atlas_db_begin(db, err);
        if (st == ATLAS_OK) {
            st = atlas_db_sem_attach_candidates(db, sum->generation_id,
                                                ATLAS_SEM_MAX_INDIRECT_CANDIDATES,
                                                &sum->candidates_attached, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_db_commit(db, err);
        } else {
            atlas_db_rollback(db);
        }
    }

    /* --- 6. publish, or fail leaving the previous generation current --- */
    sum->duration_ms = now_ms() - started;
    if (st == ATLAS_OK) {
        gen.tu_total = sum->units_total;
        gen.tu_complete = sum->units_complete;
        gen.tu_partial = sum->units_partial;
        gen.tu_failed = sum->units_failed;
        gen.tu_unsupported = sum->units_unsupported;
        /* The counts a generation reports are the rows it actually holds, not
         * the pass' running totals.
         *
         * The totals above are accumulated per translation unit, so a symbol
         * declared in a header is added once for every unit that includes it.
         * On DNA that reported 520,925 symbols and 978,122 edges for a
         * generation holding 22,305 and 325,218 — and it made a full pass and
         * an incremental one disagree by more than twenty times about identical
         * content, because the reuse path adds a different per-unit number and
         * adds no includes at all. A count an operator compares between
         * generations has to mean the same thing in both, so it is measured
         * once, here, from the rows.
         *
         * The per-unit totals are kept in the summary: "how much work this pass
         * did" is a real question, and a different one from "how big the index
         * is". */
        int64_t real_symbols = 0;
        int64_t real_edges = 0;
        int64_t real_includes = 0;
        st = atlas_db_sem_generation_counts(db, sum->generation_id, &real_symbols, &real_edges,
                                            &real_includes, err);
        if (st == ATLAS_OK) {
            sum->symbols = real_symbols;
            sum->edges = real_edges;
            sum->includes = real_includes;
        }
        gen.symbol_count = sum->symbols;
        gen.edge_count = sum->edges;
        gen.include_count = sum->includes;
        gen.duration_ms = sum->duration_ms;

        /* --- A9.2.3: the coverage manifest ---
         *
         * `tu_complete == tu_total` says every translation unit the compilation
         * database named was parsed. It says nothing about whether the
         * compilation database named every source in the repository — so on its
         * own it is a statement about the denominator's own contents, and
         * reading it as coverage was the overclaim this season exists to end.
         *
         * The denominator Atlas can state is the one A0/A1 established by
         * enumerating the tree, and `scope_discovery` records whether that
         * enumeration was one Atlas can vouch for at this moment. A file index
         * that is not current has not stopped being an enumeration — it has
         * stopped being an enumeration of *this* tree, and a candidate count
         * taken from it would be a denominator for a repository that has moved.
         * UNKNOWN is then the honest value and it is never sufficient for an
         * absence.
         *
         * Computed here, immediately before the publishing transaction, so the
         * manifest describes the generation that is about to become current
         * rather than one measured minutes earlier. */
        if (st == ATLAS_OK) {
            atlas_index_state fs;
            atlas_index_state_init(&fs);
            atlas_status fst = atlas_db_index_state_get(db, repo_id, &fs, err);
            bool file_current = fst == ATLAS_OK && atlas_index_state_is_current(&fs, NULL);
            atlas_index_state_free(&fs);
            st = fst;
            if (st == ATLAS_OK) {
                gen.scope_discovery =
                    file_current ? ATLAS_SEM_SCOPE_DECLARED : ATLAS_SEM_SCOPE_UNKNOWN;
                st = atlas_db_sem_scope_counts(db, repo_id, sum->generation_id,
                                               &gen.scope_candidates, &gen.scope_covered, err);
            }
            if (st == ATLAS_OK) {
                /* Clamped at zero rather than allowed to go negative: a unit
                 * whose source the file index does not hold — a generated
                 * source under a build directory, say — is coverage the
                 * denominator never counted, and it must not make the shortfall
                 * look smaller than it is. */
                gen.scope_uncovered = gen.scope_candidates > gen.scope_covered
                                          ? gen.scope_candidates - gen.scope_covered
                                          : 0;
                st = atlas_db_sem_scope_test_split(db, sum->generation_id, opts->test_roots,
                                                   &gen.tu_test, &gen.tu_production,
                                                   &gen.test_scope_known, err);
            }
            /* --- A9.2.4: what the *input universe* looked like ---
             *
             * The axis underneath the one above. `scope_uncovered = 0` says
             * every source the file index enumerated was read; it says nothing
             * about whether Atlas found every compilation database that could
             * have named more. That is what `discovery` records, and it is
             * sealed here for the same reason the rest of the manifest is: it
             * describes the generation about to become current, not the state of
             * the repository at some later read.
             *
             * The verdict comes from the stored discovery pass rather than from
             * a fresh walk. A pass runs before an index attempt precisely so
             * that this value is a fact about the inputs this generation was
             * built from — walking again here would record a universe the
             * generation was not built under. */
            if (st == ATLAS_OK) {
                atlas_sem_config dcfg;
                atlas_sem_config_init(&dcfg);
                if (atlas_db_sem_config_get(db, repo_id, &dcfg, err) == ATLAS_OK) {
                    gen.discovery = dcfg.discovery_state;
                    st = atlas_db_sem_scope_vendor_count(
                        db, repo_id,
                        dcfg.vendor_roots.len > 0 ? atlas_buf_cstr(&dcfg.vendor_roots) : "",
                        &gen.scope_excluded, err);
                }
                atlas_sem_config_free(&dcfg);
            }
            if (st == ATLAS_OK) {
                /* Vendored candidates are a classification, not a coverage
                 * failure: subtracting them from the shortfall is what keeps a
                 * repository with a vendored dependency able to state an absence
                 * about its own code. Clamped at zero for the reason above. */
                gen.scope_uncovered = gen.scope_uncovered > gen.scope_excluded
                                          ? gen.scope_uncovered - gen.scope_excluded
                                          : 0;
                /* The same number `compdb_count` carries, recorded again under
                 * the name the discovery model uses. The redundancy is
                 * deliberate: they are counted on different sides of the pass —
                 * one from the slots the indexer opened, one from the manifest —
                 * so a disagreement is visible rather than reconciled. */
                gen.input_count = gen.compdb_count;
            }
        }

        /* The identity this generation was built from.
         *
         * Measured *after* the pass rather than before it: what a generation may
         * claim to describe is the tree as it stood when the last unit was read,
         * and recording the identity Atlas saw at the start would claim a tree
         * the pass never finished looking at. When the tree moves mid-build the
         * consequence is the honest one — the later identity is recorded, the
         * generation publishes, and the very next freshness read compares it
         * against a tree that has moved again and reports STALE, which the
         * scheduler acts on. A generation is never published as describing a
         * state it did not observe.
         *
         * Measured **before** `atlas_db_begin`, and that placement is a rule
         * rather than a preference: computing it reads the compilation databases
         * from disk, and A1 forbids a file read inside a write transaction. The
         * transaction below writes what was measured and does no reading of its
         * own. */
        char identity[65];
        identity[0] = '\0';
        if (st == ATLAS_OK) {
            atlas_repo_info ri;
            atlas_repo_info_init(&ri);
            ri.id = repo_id;
            st = atlas_buf_set_str(&ri.root_path, opts->root, err);
            if (st == ATLAS_OK) {
                st = atlas_sem_source_identity(db, &ri, identity, err);
            }
            atlas_repo_info_free(&ri);
        }

        st = st == ATLAS_OK ? atlas_db_begin(db, err) : st;
        if (st == ATLAS_OK) {
            st = atlas_db_sem_publish(db, sum->generation_id, &gen, err);
        }
        /* Inside the publishing transaction, so the manifest and the generation
         * become visible together. Written separately from `publish` only
         * because `publish` is the compare-and-swap that names the state it
         * observed and must stay exactly that; a reader can never see a
         * published generation whose coverage is still zero and read it as
         * "nothing was covered". */
        if (st == ATLAS_OK) {
            st = atlas_db_sem_scope_set(db, sum->generation_id, &gen, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_db_sem_source_identity_set(db, sum->generation_id, identity, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_db_commit(db, err);
            sum->published = st == ATLAS_OK;
        } else {
            atlas_db_rollback(db);
        }
    }

    if (st != ATLAS_OK && sum->generation_id > 0) {
        /* Best effort, and deliberately not allowed to mask the real error: the
         * generation is marked FAILED so an operator can see the attempt, and
         * the original status is what the caller receives. */
        atlas_err ignored;
        atlas_err_init(&ignored);
        (void)atlas_db_sem_fail(db, sum->generation_id, ATLAS_SEM_WHY_CHILD_FAILED, &ignored);
        (void)snprintf(sum->failure_reason, sizeof(sum->failure_reason), "%s",
                       atlas_err_msg(err));
    }

    /* Keep a small number of superseded generations so a failed index can be
     * compared against the one still being served. Not a retention policy and
     * not on a timer — it happens here and nowhere else. */
    if (st == ATLAS_OK) {
        atlas_err ignored;
        atlas_err_init(&ignored);
        (void)atlas_db_sem_prune_generations(db, repo_id, 3, &ignored);
    }

cleanup:
    for (size_t i = 0; i < slot_count; i++) {
        slot_free(&slots[i]);
    }
    return st;
}

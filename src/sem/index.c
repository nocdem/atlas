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
#include "atlas/sem_ops.h"
#include "atlas/pathrep.h"
#include "atlas/sha256.h"

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
                                           const char *live_compdb_digest,
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
    if (running) {
        return ATLAS_SEM_FRESH_REBUILDING;
    }
    return ATLAS_SEM_FRESH_CURRENT;
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

static atlas_status load_compdb(int root_fd, const char *root, const char *rel, compdb_slot *slot,
                                atlas_err *err) {
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
    return atlas_code_compdb_parse(slot->data.data, slot->data.len, root, strlen(root),
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

static void digest_all(const compdb_slot *slots, size_t n, char out[65]) {
    char digests[ATLAS_SEM_MAX_COMPDBS][65];
    for (size_t i = 0; i < n && i < ATLAS_SEM_MAX_COMPDBS; i++) {
        (void)snprintf(digests[i], sizeof digests[i], "%s", slots[i].digest);
    }
    digest_list((const char (*)[65])digests, n, out);
}

/* --- A9.2.3: the same digest, from the files as they are now -----------------
 *
 * Until A9.2.3 every caller of `atlas_sem_freshness_of` passed NULL for the live
 * digest, so the branch that reports a changed compilation database was
 * unreachable. The reason given at the time was that recomputing it would mean
 * hashing every compilation database on every read, and that a changed one would
 * be caught by the next index.
 *
 * **That reasoning is reversed here, deliberately, and the reversal is what
 * A9.2.3 needs.** It was sound while rebuilding was something a person did:
 * "the next index" was a command somebody would run, and the check merely
 * decided how a status line read. It is not sound once the daemon owns
 * freshness, because "the next index" is now scheduled by *noticing* — so a
 * check that never fires is a repository whose build description can change
 * without anything ever rebuilding it. The dead branch was the whole of §18.
 *
 * The cost is real and bounded: one read and one SHA-256 of each declared
 * database per freshness read, of files an operator named and Atlas already
 * reads with the same bounded, symlink-refusing open the indexer uses. Nothing
 * is searched for and nothing outside the root is opened.
 *
 * A database that cannot be read now yields an empty digest, and an empty live
 * digest does not make a generation stale — "Atlas could not look" is not
 * evidence that the description changed, which is the same asymmetry A9.2.2
 * applies to absence. It is reported through the ordinary index attempt, where
 * it is a failure with a reason, rather than through a freshness read that has
 * nowhere to put it. */
atlas_status atlas_sem_live_compdb_digest(int root_fd, const char *compdbs, size_t compdbs_len,
                                          char out[65], atlas_err *err) {
    out[0] = '\0';
    if (compdbs == NULL || compdbs_len == 0 || compdbs[0] == '\0') {
        return ATLAS_OK;
    }
    char digests[ATLAS_SEM_MAX_COMPDBS][65];
    size_t n = 0;
    const char *p = compdbs;
    const char *end = compdbs + compdbs_len;
    while (p < end && *p != '\0') {
        if (n >= ATLAS_SEM_MAX_COMPDBS) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "more than %d compilation databases were named",
                                 ATLAS_SEM_MAX_COMPDBS);
        }
        atlas_buf data = ATLAS_BUF_INIT;
        atlas_status st = read_bounded(root_fd, p, ATLAS_CODE_MAX_COMPILE_DB_BYTES, &data, err);
        if (st != ATLAS_OK) {
            atlas_buf_free(&data);
            /* Unreadable now: report the empty digest rather than a different
             * one. A digest computed from what could be read would differ from
             * the stored one and report a change that may not have happened. */
            out[0] = '\0';
            return st;
        }
        atlas_sha256_hex(data.data, data.len, digests[n]);
        atlas_buf_free(&data);
        n++;
        p += strlen(p) + 1;
    }
    digest_list((const char (*)[65])digests, n, out);
    return ATLAS_OK;
}

atlas_status atlas_sem_repo_compdb_digest(atlas_db *db, atlas_repo_info *repo, char out[65],
                                          atlas_err *err) {
    out[0] = '\0';
    atlas_sem_config cfg;
    atlas_sem_config_init(&cfg);
    atlas_status st = atlas_db_sem_config_get(db, repo->id, &cfg, err);
    if (st != ATLAS_OK || !cfg.present || cfg.compdbs.len == 0) {
        /* No declared build description, so there is nothing to compare against
         * and no claim to make. An empty digest never makes a generation stale. */
        atlas_sem_config_free(&cfg);
        return st;
    }
    atlas_buf list = ATLAS_BUF_INIT;
    st = atlas_sem_config_unpack(atlas_buf_cstr(&cfg.compdbs), &list, NULL, err);
    if (st == ATLAS_OK) {
        /* The registered root, opened without following a symlink on its final
         * component: a root that has been replaced by a link since registration
         * refuses the read rather than being followed somewhere else. Every
         * descent below it is `atlas_path_open_nofollow`, which is the same
         * discipline the indexer uses. No git process is created — this runs on
         * a read path, and forking git to hash two files an operator named would
         * make every status read cost a process. */
        int fd = open(atlas_buf_cstr(&repo->root_path),
                      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) {
            st = atlas_err_set_errno(err, ATLAS_ERR_REPO, errno,
                                     "cannot open the registered repository root");
        } else {
            st = atlas_sem_live_compdb_digest(fd, (const char *)list.data, list.len, out, err);
            (void)close(fd);
        }
    }
    atlas_buf_free(&list);
    atlas_sem_config_free(&cfg);
    return st;
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
    static const char DOMAIN[] = "atlas.sem.unit.v1";
    atlas_sha256_update(&h, DOMAIN, sizeof(DOMAIN));
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
                atlas_sha256_update(&h, "\x00absent", 7);
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
        st = load_compdb(opts->root_fd, opts->root, p, &slots[slot_count], err);
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
                    if (st == ATLAS_OK) {
                        st = atlas_db_sem_copy_unit(db, prev_gen, sum->generation_id, source_rel,
                                                    config, &syms, &edges, err);
                    }
                    if (st == ATLAS_OK) {
                        atlas_sem_unit_row row;
                        memset(&row, 0, sizeof(row));
                        row.generation_id = sum->generation_id;
                        row.source_text = source_rel;
                        row.compdb_id = slots[si].row_id;
                        row.config_digest = config;
                        row.input_digest = want;
                        row.status = ATLAS_SEM_TU_COMPLETE;
                        row.symbols = syms;
                        row.edges = edges;
                        row.reused = true;
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

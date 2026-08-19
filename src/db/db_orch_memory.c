/* Atlas - A10.1: freezing and reading one run's cross-run memory manifest.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The reads here gather candidates; the pure part — scoring, ordering, bounding
 * and rendering — is `src/orch/memory.c` and is deliberately somewhere a
 * database handle cannot reach. That split is what lets a reader re-derive a
 * frozen package from the stored candidates and compare digests.
 *
 * Everything in this file runs inside the submit transaction that creates the
 * run, so: no git process, no file read, no clock beyond the one timestamp the
 * row needs, and every scan bounded by a compiled-in ceiling. A11.1's
 * transaction rules apply here exactly as they do to every other write path.
 */
#define _GNU_SOURCE 1

#include <stdio.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/limits.h"
#include "atlas/orch_memory.h"
#include "atlas/orch_ops.h"
#include "atlas/sha256.h"
#include "db_internal.h"

/* --- lineage ---------------------------------------------------------------
 *
 * The question memory asks is "is this the same git history?", and the answer
 * is the object format plus the sorted set of ingested root commits — the half
 * of A4's identity that is not the path.
 *
 * It has its own domain and its own name because it is its own value. It is
 * never a redescription of `repo_identity_hash`, which is path-qualified and is
 * what every authorisation, admission and refusal in Atlas continues to use.
 * Nothing is decided on this one: it selects hints.
 *
 * A repository with no ingested root commit has no lineage. Reported as empty
 * rather than as a hash of the object format alone, which would look like an
 * identity and would match every other unscanned repository. */
static atlas_status lineage_of(atlas_db *db, int64_t repo_id, atlas_buf *out, atlas_err *err) {
    atlas_buf_reset(out);
    sqlite3_stmt *s = NULL;
    atlas_status st =
        atlas_db_prepare(db, "SELECT object_format FROM repositories WHERE id = ?1;", &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    (void)sqlite3_bind_int64(s, 1, repo_id);
    char format[64];
    format[0] = '\0';
    bool found = false;
    if (sqlite3_step(s) == SQLITE_ROW) {
        found = true;
        st = atlas_db_col_copy(s, 0, format, sizeof format, "object_format", err);
    }
    atlas_db_finish(db, s);
    if (st != ATLAS_OK || !found) {
        return st;
    }

    sqlite3_stmt *c = NULL;
    st = atlas_db_prepare(db,
                          "SELECT oid FROM commits WHERE repo_id = ?1 AND parent_count = 0"
                          " ORDER BY oid LIMIT 64;",
                          &c, err);
    if (st != ATLAS_OK) {
        return st;
    }
    (void)sqlite3_bind_int64(c, 1, repo_id);
    atlas_sha256 ctx;
    atlas_sha256_init(&ctx);
    static const char domain[] = "atlas.orch.memory.lineage.v1";
    atlas_sha256_update(&ctx, domain, sizeof(domain));
    atlas_sha256_update(&ctx, format, strlen(format));
    atlas_sha256_update(&ctx, "|", 1u);
    int64_t roots = 0;
    while (sqlite3_step(c) == SQLITE_ROW) {
        const char *oid = atlas_db_col_text(c, 0);
        atlas_sha256_update(&ctx, oid, strlen(oid));
        atlas_sha256_update(&ctx, ",", 1u);
        roots++;
    }
    atlas_db_finish(db, c);
    if (roots == 0) {
        return ATLAS_OK;
    }
    unsigned char digest[ATLAS_SHA256_DIGEST_LEN];
    atlas_sha256_final(&ctx, digest);
    char hex[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_hex_encode(digest, sizeof(digest), hex);
    return atlas_buf_set_str(out, hex, err);
}

/* The lineage a stored `repo_identity_hash` belongs to.
 *
 * A run stores the path-qualified identity, so the only way back to a lineage
 * is through a live registry row that still has that identity. A run whose
 * repository has been removed or moved resolves to nothing and is therefore not
 * a candidate — absent, never guessed, and never widened to "some repository
 * that looks similar".
 *
 * Bounded by the registry, which an operator maintains by hand. */
static atlas_status lineage_for_identity(atlas_db *db, const char *identity, atlas_buf *out,
                                         atlas_err *err) {
    atlas_buf_reset(out);
    if (identity == NULL || identity[0] == '\0') {
        return ATLAS_OK;
    }
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(db, "SELECT id FROM repositories ORDER BY id;", &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    int64_t ids[64];
    size_t n = 0;
    while (n < sizeof ids / sizeof ids[0] && sqlite3_step(s) == SQLITE_ROW) {
        ids[n++] = sqlite3_column_int64(s, 0);
    }
    atlas_db_finish(db, s);

    atlas_buf ih = ATLAS_BUF_INIT;
    for (size_t i = 0; st == ATLAS_OK && i < n; i++) {
        st = atlas_db_repo_identity_hash(db, ids[i], &ih, err);
        if (st == ATLAS_OK && ih.len > 0 && strcmp(atlas_buf_cstr(&ih), identity) == 0) {
            st = lineage_of(db, ids[i], out, err);
            break;
        }
    }
    atlas_buf_free(&ih);
    return st;
}

/* --- gathering -------------------------------------------------------------- */

static atlas_status set_from_col(atlas_buf *b, sqlite3_stmt *s, int col, atlas_err *err) {
    const char *t = atlas_db_col_text_opt(s, col);
    return atlas_buf_set_str(b, t != NULL ? t : "", err);
}

/* Renders one run's declared gates from the stored netstring list, one per
 * line. The argv came from an operator's `--gate` and is on the binary's own
 * program allowlist, so it is Atlas-checked rather than model-produced — but it
 * is still rendered through the same bounded path as everything else here. */
static atlas_status gates_render(const char *encoded, atlas_buf *out, atlas_err *err) {
    atlas_orch_argv cmds[ATLAS_ORCH_MAX_VALIDATIONS];
    for (size_t i = 0; i < ATLAS_ORCH_MAX_VALIDATIONS; i++) {
        atlas_orch_argv_init(&cmds[i]);
    }
    size_t n = 0;
    atlas_status st = atlas_orch_validations_decode(encoded != NULL ? encoded : "", cmds,
                                                    ATLAS_ORCH_MAX_VALIDATIONS, &n, err);
    for (size_t i = 0; st == ATLAS_OK && i < n; i++) {
        if (i > 0) {
            st = atlas_buf_append_str(out, "\n", err);
        }
        for (size_t k = 0; st == ATLAS_OK && k < cmds[i].count; k++) {
            if (k > 0) {
                st = atlas_buf_append_str(out, " ", err);
            }
            if (st == ATLAS_OK) {
                st = atlas_buf_append(out, cmds[i].args[k].data, cmds[i].args[k].len, err);
            }
        }
    }
    for (size_t i = 0; i < ATLAS_ORCH_MAX_VALIDATIONS; i++) {
        atlas_orch_argv_free(&cmds[i]);
    }
    if (st != ATLAS_OK) {
        /* A gate list that will not decode is left out of the entry rather than
         * refusing the whole package. It is a hint about a hint. */
        atlas_err_init(err);
        atlas_buf_reset(out);
        st = ATLAS_OK;
    }
    return st;
}

/* The bounded excerpt of what a run's failing gate printed, taken from the
 * `gate.log` artifact of the run's last terminal attempt.
 *
 * `worker.log` is deliberately not read. It is the whole streamed transcript —
 * prompts, tool arguments, model prose — and none of that may enter a package.
 * `gate.log` is the output of a compiler or a test runner over a tree, which is
 * the one piece of evidence about a past failure that is both bounded and
 * genuinely useful. */
static atlas_status detail_of(atlas_db *db, const char *run_uid, atlas_buf *out, atlas_err *err) {
    static const char SQL[] =
        "SELECT a.content FROM orch_artifacts a"
        "  JOIN orch_jobs j ON j.id = a.job_id"
        " WHERE j.run_uid = ?1 AND a.name = 'gate.log' AND a.content_stored = 1"
        " ORDER BY a.id DESC LIMIT 1;";
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_bind_text_opt(db, s, 1, run_uid, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_step(s) == SQLITE_ROW) {
        const void *blob = sqlite3_column_blob(s, 0);
        int len = sqlite3_column_bytes(s, 0);
        if (blob != NULL && len > 0) {
            /* The tail, not the head. A build log's last lines are the ones
             * carrying the error; its first are the ones carrying the banner. */
            size_t take = (size_t)len;
            size_t off = 0;
            if (take > 2048u) {
                off = take - 2048u;
                take = 2048u;
            }
            st = atlas_buf_set(out, (const char *)blob + off, take, err);
        }
    }
    atlas_db_finish(db, s);
    return st;
}

/* Gathers at most ATLAS_ORCH_MEMORY_MAX_CANDIDATES terminal runs sharing the
 * requesting repository's lineage, newest first.
 *
 * Two exclusions, and they answer different halves of the same worry.
 *
 * **Only runs that were already terminal when this ran.** A run created and not
 * yet finished is ACTIVE, and an ACTIVE run is invisible here whatever it later
 * becomes. Combined with freezing at run creation, a package cannot contain a
 * result that did not exist when it was frozen.
 *
 * **And never a run that carries a memory manifest of its own.** The freeze
 * ordering above is only enough while every arm is created before any arm runs,
 * and a comparison of several pairs cannot do that: a task's wall deadline is
 * `created_ms + wall_timeout_ms`, so a run submitted and left queued past it is
 * timed out and its run blocked. A later pair therefore has to be created after
 * an earlier pair has finished — at which point the earlier pair's runs are
 * terminal, share the lineage, and share most of their vocabulary.
 *
 * A run with a manifest was created by an invocation that made a deliberate
 * choice about memory, so it is part of a memory arm — either arm. Excluding it
 * is one predicate with no list of identifiers in it, and it is what makes "the
 * runs this experiment created are not candidates" a property of the query
 * rather than of the order somebody ran things in.
 *
 * The cost is stated rather than hidden: **bounded memory does not compound.**
 * A run that was shown memory does not itself become memory, so the candidate
 * universe stays the runs that predate this mechanism. For a milestone whose
 * whole purpose is to measure the mechanism that is the conservative direction
 * — a corpus already shaped by memory cannot measure memory — and it is the
 * first thing to revisit if the answer turns out to be that memory helps. */
static atlas_status gather(atlas_db *db, const char *lineage, const char *exclude_run,
                           atlas_orch_memory_cand *out, size_t max, size_t *n_out,
                           bool *truncated, atlas_err *err) {
    *n_out = 0;
    *truncated = false;
    static const char SQL[] =
        "SELECT r.run_uid, r.status, r.repo_identity_hash, r.created_ms,"
        "       j.source_commit, j.task_text, j.validations"
        "  FROM orch_runs r JOIN orch_jobs j ON j.job_uid = r.root_job_uid"
        " WHERE r.status IN ('ACCEPTED','BLOCKED') AND r.run_uid <> ?1"
        "   AND NOT EXISTS (SELECT 1 FROM orch_run_memory m WHERE m.run_uid = r.run_uid)"
        " ORDER BY r.id DESC LIMIT ?2;";
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_bind_text_opt(db, s, 1, exclude_run != NULL ? exclude_run : "", err);
    if (st != ATLAS_OK) {
        return st;
    }
    (void)sqlite3_bind_int64(s, 2, (sqlite3_int64)ATLAS_ORCH_MEMORY_MAX_CANDIDATES);

    atlas_buf cand_lineage = ATLAS_BUF_INIT;
    size_t seen = 0;
    while (st == ATLAS_OK && sqlite3_step(s) == SQLITE_ROW) {
        seen++;
        if (*n_out >= max) {
            *truncated = true;
            break;
        }
        const char *identity = atlas_db_col_text(s, 2);
        st = lineage_for_identity(db, identity, &cand_lineage, err);
        if (st != ATLAS_OK) {
            break;
        }
        if (cand_lineage.len == 0 || lineage == NULL || lineage[0] == '\0' ||
            strcmp(atlas_buf_cstr(&cand_lineage), lineage) != 0) {
            continue;
        }
        atlas_orch_memory_cand *c = &out[*n_out];
        atlas_orch_memory_cand_init(c);
        st = atlas_db_col_copy(s, 0, c->run_uid, sizeof c->run_uid, "run_uid", err);
        if (st == ATLAS_OK) {
            st = atlas_db_col_copy(s, 1, c->status, sizeof c->status, "status", err);
        }
        if (st == ATLAS_OK) {
            st = atlas_db_col_copy(s, 4, c->source_commit, sizeof c->source_commit,
                                   "source_commit", err);
        }
        if (st == ATLAS_OK) {
            c->created_ms = sqlite3_column_int64(s, 3);
            st = set_from_col(&c->goal, s, 5, err);
        }
        if (st == ATLAS_OK) {
            st = gates_render(atlas_db_col_text_opt(s, 6), &c->gates, err);
        }
        if (st == ATLAS_OK) {
            (*n_out)++;
        } else {
            atlas_orch_memory_cand_free(c);
        }
    }
    if (seen >= ATLAS_ORCH_MEMORY_MAX_CANDIDATES) {
        *truncated = true;
    }
    atlas_db_finish(db, s);
    atlas_buf_free(&cand_lineage);
    return st;
}

/* The per-run facts that need their own queries: how many worker starts and
 * tasks the run spent, how its last task ended, and what it cost. */
static atlas_status enrich(atlas_db *db, atlas_orch_memory_cand *c, const char *current_commit,
                           atlas_err *err) {
    static const char COUNTS[] =
        "SELECT (SELECT count(*) FROM orch_transitions t JOIN orch_jobs j ON j.id = t.job_id"
        "         WHERE j.run_uid = ?1 AND t.to_state = 'RUNNING'),"
        "       (SELECT count(*) FROM orch_jobs j WHERE j.run_uid = ?1);";
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(db, COUNTS, &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_bind_text_opt(db, s, 1, c->run_uid, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_step(s) == SQLITE_ROW) {
        c->worker_starts = sqlite3_column_int64(s, 0);
        c->task_count = sqlite3_column_int64(s, 1);
    }
    atlas_db_finish(db, s);

    static const char LAST[] =
        "SELECT a.failure_reason FROM orch_attempts a JOIN orch_jobs j ON j.id = a.job_id"
        " WHERE j.run_uid = ?1 ORDER BY a.id DESC LIMIT 1;";
    if (st == ATLAS_OK) {
        sqlite3_stmt *q = NULL;
        st = atlas_db_prepare(db, LAST, &q, err);
        if (st == ATLAS_OK) {
            st = atlas_db_bind_text_opt(db, q, 1, c->run_uid, err);
        }
        if (st == ATLAS_OK) {
            if (sqlite3_step(q) == SQLITE_ROW) {
                st = set_from_col(&c->terminal_reason, q, 0, err);
            }
            atlas_db_finish(db, q);
        }
    }
    if (st == ATLAS_OK) {
        st = detail_of(db, c->run_uid, &c->detail, err);
    }
    if (st == ATLAS_OK) {
        atlas_status us = atlas_db_orch_run_usage(db, c->run_uid, &c->usage, err);
        if (us == ATLAS_OK) {
            c->usage_present = true;
        } else {
            atlas_err_init(err);
        }
    }
    if (st == ATLAS_OK) {
        /* The commit relation. EXACT is settled by the pure builder, which
         * compares the strings; INDEXED is a fact about this database that only
         * a query can answer. Neither is an ancestry claim. */
        c->rel = ATLAS_ORCH_MEMORY_COMMIT_UNKNOWN;
        if (current_commit != NULL && c->source_commit[0] != '\0' &&
            strcmp(current_commit, c->source_commit) == 0) {
            c->rel = ATLAS_ORCH_MEMORY_COMMIT_EXACT;
        } else if (c->source_commit[0] != '\0') {
            sqlite3_stmt *q = NULL;
            st = atlas_db_prepare(db, "SELECT 1 FROM commits WHERE oid = ?1 LIMIT 1;", &q, err);
            if (st == ATLAS_OK) {
                st = atlas_db_bind_text_opt(db, q, 1, c->source_commit, err);
            }
            if (st == ATLAS_OK) {
                if (sqlite3_step(q) == SQLITE_ROW) {
                    c->rel = ATLAS_ORCH_MEMORY_COMMIT_INDEXED;
                }
                atlas_db_finish(db, q);
            }
        }
    }
    return st;
}

atlas_status atlas_db_orch_memory_freeze(atlas_db *db, const char *run_uid,
                                         atlas_orch_memory_mode mode, int64_t repo_id,
                                         const char *task_text, const char *current_commit,
                                         atlas_orch_memory_package *out, atlas_err *err) {
    atlas_orch_memory_cand cands[ATLAS_ORCH_MEMORY_MAX_CANDIDATES];
    size_t n = 0;
    bool truncated = false;
    atlas_buf lineage = ATLAS_BUF_INIT;
    atlas_status st = ATLAS_OK;

    if (mode == ATLAS_ORCH_MEMORY_MODE_BOUNDED) {
        st = lineage_of(db, repo_id, &lineage, err);
        if (st == ATLAS_OK) {
            st = gather(db, atlas_buf_cstr(&lineage), run_uid, cands,
                        ATLAS_ORCH_MEMORY_MAX_CANDIDATES, &n, &truncated, err);
        }
        for (size_t i = 0; st == ATLAS_OK && i < n; i++) {
            st = enrich(db, &cands[i], current_commit, err);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_orch_memory_build(mode, task_text, current_commit, cands, n, truncated, out,
                                     err);
    }
    for (size_t i = 0; i < n; i++) {
        atlas_orch_memory_cand_free(&cands[i]);
    }
    atlas_buf_free(&lineage);
    if (st != ATLAS_OK) {
        return st;
    }

    /* The freeze. `UNIQUE(run_uid)` is what makes it one, and a plain INSERT is
     * what makes a second attempt fail loudly rather than replace what an
     * already-started arm was shown. */
    static const char INS[] =
        "INSERT INTO orch_run_memory(run_uid, mode, status, digest, bytes, source_count,"
        "  manifest, candidates_truncated, package, created_at)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10);";
    sqlite3_stmt *s = NULL;
    st = atlas_db_prepare(db, INS, &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char at[ATLAS_TS_MAX];
    atlas_now_iso8601(at, sizeof(at));
    const char *texts[] = {run_uid, atlas_orch_memory_mode_name(mode),
                           atlas_orch_memory_status_name(out->status), out->digest};
    for (size_t i = 0; st == ATLAS_OK && i < sizeof texts / sizeof texts[0]; i++) {
        st = atlas_db_bind_text_opt(db, s, (int)i + 1, texts[i], err);
    }
    if (st == ATLAS_OK) {
        (void)sqlite3_bind_int64(s, 5, (sqlite3_int64)out->bytes);
        (void)sqlite3_bind_int64(s, 6, (sqlite3_int64)out->source_count);
        st = atlas_db_bind_text_opt(db, s, 7, atlas_buf_cstr(&out->manifest), err);
    }
    if (st == ATLAS_OK) {
        (void)sqlite3_bind_int64(s, 8, out->candidates_truncated ? 1 : 0);
        (void)sqlite3_bind_blob(s, 9, out->package.data != NULL ? out->package.data : "",
                                (int)out->package.len, SQLITE_TRANSIENT);
        st = atlas_db_bind_text_opt(db, s, 10, at, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_step_done(db, s, err);
    } else {
        atlas_db_finish(db, s);
    }
    return st;
}

atlas_status atlas_db_orch_memory_get(atlas_db *db, const char *run_uid,
                                      atlas_orch_memory_package *out, bool *found,
                                      atlas_orch_memory_mode *mode_out, atlas_err *err) {
    *found = false;
    *mode_out = ATLAS_ORCH_MEMORY_MODE_UNKNOWN;
    out->status = ATLAS_ORCH_MEMORY_PKG_UNKNOWN;
    if (run_uid == NULL || run_uid[0] == '\0') {
        return ATLAS_OK;
    }
    static const char SQL[] =
        "SELECT mode, status, digest, bytes, source_count, manifest, candidates_truncated, package"
        "  FROM orch_run_memory WHERE run_uid = ?1;";
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_bind_text_opt(db, s, 1, run_uid, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_step(s) == SQLITE_ROW) {
        *found = true;
        const char *m = atlas_db_col_text(s, 0);
        const char *k = atlas_db_col_text(s, 1);
        if (!atlas_orch_memory_mode_parse(m, mode_out)) {
            st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                               "run %s holds a memory mode Atlas does not recognise", run_uid);
        }
        if (st == ATLAS_OK && !atlas_orch_memory_status_parse(k, &out->status)) {
            st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                               "run %s holds a memory package status Atlas does not recognise",
                               run_uid);
        }
        if (st == ATLAS_OK) {
            (void)snprintf(out->digest, sizeof out->digest, "%s", atlas_db_col_text(s, 2));
            out->bytes = (size_t)sqlite3_column_int64(s, 3);
            out->source_count = (size_t)sqlite3_column_int64(s, 4);
            if (out->source_count > ATLAS_ORCH_MEMORY_MAX_SOURCES) {
                out->source_count = ATLAS_ORCH_MEMORY_MAX_SOURCES;
            }
            out->candidates_truncated = sqlite3_column_int64(s, 6) != 0;
            st = atlas_buf_set_str(&out->manifest, atlas_db_col_text(s, 5), err);
        }
        if (st == ATLAS_OK) {
            const void *blob = sqlite3_column_blob(s, 7);
            int len = sqlite3_column_bytes(s, 7);
            st = atlas_buf_set(&out->package, blob != NULL ? (const char *)blob : "",
                               len > 0 ? (size_t)len : 0u, err);
        }
        /* The source uids are re-read from the manifest rather than kept in a
         * second column, so there is one place they are recorded and no way for
         * the two to disagree. */
        if (st == ATLAS_OK) {
            const char *p = atlas_buf_cstr(&out->manifest);
            size_t field = 0, taken = 0;
            while (*p != '\0' && taken < ATLAS_ORCH_MEMORY_MAX_SOURCES) {
                size_t len = 0;
                const char *q = p;
                while (*q >= '0' && *q <= '9') {
                    len = len * 10u + (size_t)(*q - '0');
                    q++;
                }
                if (*q != ':' || len > 4096u) {
                    break;
                }
                q++;
                if (strlen(q) < len + 1u || q[len] != ',') {
                    break;
                }
                if (field % 6u == 0u && len < ATLAS_ORCH_RUN_UID_MAX) {
                    memcpy(out->sources[taken], q, len);
                    out->sources[taken][len] = '\0';
                    taken++;
                }
                field++;
                p = q + len + 1u;
            }
        }
    }
    atlas_db_finish(db, s);
    return st;
}

/* Atlas - typed operations over the migration-11 semantic tables.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Every statement here is a string literal, so `atlas_db_prepare`'s pointer
 * cache behaves as its header documents. Every read is scoped to a generation
 * id, and a generation belongs to exactly one repository — which is what makes
 * "no query can reach another repository's rows" a property of the shape of
 * this file rather than a rule callers must remember.
 */
#include "atlas/sem_ops.h"

#include <string.h>

#include "atlas/atlas.h"
#include "atlas/sem_discover.h"
#include "atlas/sha256.h"
#include "atlas/syspolicy.h"
#include "db_internal.h"

/* --- small helpers ---------------------------------------------------------- */

static const char *nz(const char *s) { return s == NULL ? "" : s; }

static atlas_status bind_text(atlas_db *db, sqlite3_stmt *stmt, int idx, const char *v,
                              atlas_err *err) {
    return atlas_db_bind_text_opt(db, stmt, idx, nz(v), err);
}

/* Binds a buffer's exact bytes. Separate from `bind_text` because an
 * `atlas_buf` is not necessarily NUL-terminated and `atlas_buf_cstr` would
 * terminate it, which needs a writable handle on something a writer has no
 * business modifying. */
static atlas_status bind_buf(atlas_db *db, sqlite3_stmt *stmt, int idx, const atlas_buf *b,
                             atlas_err *err) {
    const char *p = b->len > 0 ? (const char *)b->data : "";
    if (sqlite3_bind_text(stmt, idx, p, (int)b->len, SQLITE_TRANSIENT) != SQLITE_OK) {
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind a text value");
    }
    return ATLAS_OK;
}

static const char *col_text(sqlite3_stmt *stmt, int idx) {
    const unsigned char *t = sqlite3_column_text(stmt, idx);
    return t == NULL ? "" : (const char *)t;
}

static void copy_field(char *dst, size_t cap, const char *src) {
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* --- generations ------------------------------------------------------------ */

atlas_status atlas_db_sem_generation_begin(atlas_db *db, const atlas_sem_generation *g,
                                           int64_t *id_out, atlas_err *err) {
    if (g == NULL || id_out == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "semantic generation: bad request");
    }
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "INSERT INTO sem_generations(repo_id, repo_identity_hash, commit_id, compdb_digest,"
        "  compdb_count, compiler_id, compiler_version, analyzer_id, analyzer_version,"
        "  status, started_at)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,'RUNNING',?10);",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, g->repo_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 5, g->compdb_count) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 9, g->analyzer_version) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind semantic generation");
    }
    st = bind_text(db, stmt, 2, g->repo_identity_hash, err);
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 3, g->commit_id, err);
    }
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 4, g->compdb_digest, err);
    }
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 6, g->compiler_id, err);
    }
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 7, g->compiler_version, err);
    }
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 8, g->analyzer_id, err);
    }
    if (st == ATLAS_OK) {
        char now[ATLAS_TS_MAX];
        atlas_now_iso8601(now, sizeof(now));
        st = bind_text(db, stmt, 10, now, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    st = atlas_db_step_done(db, stmt, err);
    if (st == ATLAS_OK) {
        *id_out = sqlite3_last_insert_rowid(db->h);
    }
    return st;
}

atlas_status atlas_db_sem_publish(atlas_db *db, int64_t generation_id,
                                  const atlas_sem_generation *c, atlas_err *err) {
    if (c == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "semantic publish: bad request");
    }
    /* Compare-and-swap on the state it observed, and exactly one row must
     * change — A4's rule, so two passes racing to publish lose deterministically
     * instead of last-write-wins. */
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "UPDATE sem_generations SET status = 'COMPLETE', completed_at = ?2, tu_total = ?3,"
        "  tu_complete = ?4, tu_partial = ?5, tu_failed = ?6, tu_unsupported = ?7,"
        "  symbol_count = ?8, edge_count = ?9, include_count = ?10, duration_ms = ?11"
        " WHERE id = ?1 AND status = 'RUNNING';",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));
    if (sqlite3_bind_int64(stmt, 1, generation_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 3, c->tu_total) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 4, c->tu_complete) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 5, c->tu_partial) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 6, c->tu_failed) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 7, c->tu_unsupported) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 8, c->symbol_count) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 9, c->edge_count) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 10, c->include_count) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 11, c->duration_ms) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind semantic publish");
    }
    st = bind_text(db, stmt, 2, now, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    st = atlas_db_step_done(db, stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_changes(db->h) != 1) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "the semantic generation was not RUNNING when it was published; "
                             "another pass changed it");
    }

    /* And the pointer, in the same transaction. Becoming visible is one act. */
    stmt = NULL;
    st = atlas_db_prepare(db,
                          "INSERT INTO sem_current(repo_id, generation_id) VALUES(?1, ?2)"
                          " ON CONFLICT(repo_id) DO UPDATE SET generation_id = excluded.generation_id;",
                          &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, c->repo_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, generation_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind semantic current pointer");
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_sem_fail(atlas_db *db, int64_t generation_id, const char *why,
                               atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "UPDATE sem_generations SET status = 'FAILED', completed_at = ?2, failure_reason = ?3"
        " WHERE id = ?1 AND status = 'RUNNING';",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));
    if (sqlite3_bind_int64(stmt, 1, generation_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind semantic failure");
    }
    st = bind_text(db, stmt, 2, now, err);
    if (st == ATLAS_OK) {
        /* Only Atlas' own vocabulary is stored. A reason from anywhere else
         * becomes the generic one rather than being reproduced. */
        st = bind_text(db, stmt, 3, atlas_sem_why_is_known(why) ? why : "", err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

static void read_generation(sqlite3_stmt *stmt, atlas_sem_generation *out) {
    atlas_sem_generation_init(out);
    out->id = sqlite3_column_int64(stmt, 0);
    out->repo_id = sqlite3_column_int64(stmt, 1);
    copy_field(out->repo_identity_hash, sizeof(out->repo_identity_hash), col_text(stmt, 2));
    copy_field(out->commit_id, sizeof(out->commit_id), col_text(stmt, 3));
    copy_field(out->compdb_digest, sizeof(out->compdb_digest), col_text(stmt, 4));
    out->compdb_count = sqlite3_column_int64(stmt, 5);
    copy_field(out->compiler_id, sizeof(out->compiler_id), col_text(stmt, 6));
    copy_field(out->compiler_version, sizeof(out->compiler_version), col_text(stmt, 7));
    copy_field(out->analyzer_id, sizeof(out->analyzer_id), col_text(stmt, 8));
    out->analyzer_version = sqlite3_column_int64(stmt, 9);
    if (!atlas_sem_gen_status_parse(col_text(stmt, 10), &out->status)) {
        out->status = ATLAS_SEM_GEN_UNKNOWN;
    }
    copy_field(out->started_at, sizeof(out->started_at), col_text(stmt, 11));
    copy_field(out->completed_at, sizeof(out->completed_at), col_text(stmt, 12));
    out->tu_total = sqlite3_column_int64(stmt, 13);
    out->tu_complete = sqlite3_column_int64(stmt, 14);
    out->tu_partial = sqlite3_column_int64(stmt, 15);
    out->tu_failed = sqlite3_column_int64(stmt, 16);
    out->tu_unsupported = sqlite3_column_int64(stmt, 17);
    out->symbol_count = sqlite3_column_int64(stmt, 18);
    out->edge_count = sqlite3_column_int64(stmt, 19);
    out->include_count = sqlite3_column_int64(stmt, 20);
    out->duration_ms = sqlite3_column_int64(stmt, 21);
    copy_field(out->failure_reason, sizeof(out->failure_reason), col_text(stmt, 22));
    /* A9.2.3's manifest. An unparseable value leaves UNKNOWN, which is the zero
     * and the safe reading: a generation whose scope Atlas cannot read is not
     * one whose coverage may support an absence. */
    if (!atlas_sem_scope_discovery_parse(col_text(stmt, 23), &out->scope_discovery)) {
        out->scope_discovery = ATLAS_SEM_SCOPE_UNKNOWN;
    }
    out->scope_candidates = sqlite3_column_int64(stmt, 24);
    out->scope_covered = sqlite3_column_int64(stmt, 25);
    out->scope_uncovered = sqlite3_column_int64(stmt, 26);
    out->tu_test = sqlite3_column_int64(stmt, 27);
    out->tu_production = sqlite3_column_int64(stmt, 28);
    out->test_scope_known = sqlite3_column_int64(stmt, 29) != 0;
    copy_field(out->source_identity, sizeof(out->source_identity), col_text(stmt, 30));
    /* A9.2.4. An unrecognised stored value leaves UNKNOWN, which is the reading
     * that supports no absence. */
    (void)atlas_sem_discovery_parse(col_text(stmt, 31), &out->discovery);
    out->input_count = sqlite3_column_int64(stmt, 32);
    out->scope_excluded = sqlite3_column_int64(stmt, 33);
}

#define GEN_COLUMNS                                                                             \
    "g.id, g.repo_id, g.repo_identity_hash, g.commit_id, g.compdb_digest, g.compdb_count,"      \
    " g.compiler_id, g.compiler_version, g.analyzer_id, g.analyzer_version, g.status,"          \
    " g.started_at, g.completed_at, g.tu_total, g.tu_complete, g.tu_partial, g.tu_failed,"      \
    " g.tu_unsupported, g.symbol_count, g.edge_count, g.include_count, g.duration_ms,"          \
    " g.failure_reason, g.scope_discovery, g.scope_candidates, g.scope_covered,"                \
    " g.scope_uncovered, g.tu_test, g.tu_production, g.test_scope_known,"                \
    " g.source_identity, g.discovery, g.input_count, g.scope_excluded"

atlas_status atlas_db_sem_current(atlas_db *db, int64_t repo_id, atlas_sem_generation *out,
                                  bool *found, atlas_err *err) {
    *found = false;
    sqlite3_stmt *stmt = NULL;
    atlas_status st =
        atlas_db_prepare(db,
                         "SELECT " GEN_COLUMNS " FROM sem_generations g"
                         " JOIN sem_current c ON c.generation_id = g.id"
                         " WHERE c.repo_id = ?1;",
                         &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        read_generation(stmt, out);
        out->is_current = true;
        *found = true;
    } else if (rc != SQLITE_DONE) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read the current semantic generation");
    }
    atlas_db_finish(db, stmt);
    return ATLAS_OK;
}

atlas_status atlas_db_sem_latest(atlas_db *db, int64_t repo_id, atlas_sem_generation *out,
                                 bool *found, atlas_err *err) {
    *found = false;
    sqlite3_stmt *stmt = NULL;
    /* Ordered by id, never by a timestamp — A8's rule. */
    atlas_status st = atlas_db_prepare(db,
                                       "SELECT " GEN_COLUMNS " FROM sem_generations g"
                                       " WHERE g.repo_id = ?1 ORDER BY g.id DESC LIMIT 1;",
                                       &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        read_generation(stmt, out);
        *found = true;
    } else if (rc != SQLITE_DONE) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read the latest semantic generation");
    }
    atlas_db_finish(db, stmt);
    return ATLAS_OK;
}

static atlas_status exec_gen_scoped(atlas_db *db, const char *sql, int64_t generation_id,
                                    atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, sql, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, generation_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind generation id");
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_sem_generation_delete(atlas_db *db, int64_t generation_id, atlas_err *err) {
    /* Refuses to remove what is published. Replacing an index means publishing
     * a new generation, never deleting the old one first — that ordering is
     * what keeps a reader from ever seeing no index at all. */
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db, "SELECT COUNT(*) FROM sem_current WHERE generation_id = ?1;", &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, generation_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind generation id");
    }
    int64_t published = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        published = sqlite3_column_int64(stmt, 0);
    }
    atlas_db_finish(db, stmt);
    if (published > 0) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "the published semantic generation cannot be deleted");
    }

    static const char *const SQL[] = {
        "DELETE FROM sem_edges WHERE generation_id = ?1;",
        "DELETE FROM sem_symbols WHERE generation_id = ?1;",
        "DELETE FROM sem_includes WHERE generation_id = ?1;",
        "DELETE FROM sem_units WHERE generation_id = ?1;",
        "DELETE FROM sem_compdbs WHERE generation_id = ?1;",
        "DELETE FROM sem_generations WHERE id = ?1;",
    };
    for (size_t i = 0; i < sizeof(SQL) / sizeof(SQL[0]); i++) {
        st = exec_gen_scoped(db, SQL[i], generation_id, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    return ATLAS_OK;
}

atlas_status atlas_db_sem_prune_generations(atlas_db *db, int64_t repo_id, int64_t keep,
                                            atlas_err *err) {
    if (keep < 1) {
        keep = 1;
    }
    for (;;) {
        sqlite3_stmt *stmt = NULL;
        atlas_status st = atlas_db_prepare(
            db,
            "SELECT g.id FROM sem_generations g"
            " WHERE g.repo_id = ?1"
            "   AND g.id NOT IN (SELECT generation_id FROM sem_current)"
            "   AND g.id NOT IN (SELECT id FROM sem_generations WHERE repo_id = ?1"
            "                     ORDER BY id DESC LIMIT ?2)"
            " ORDER BY g.id ASC LIMIT 1;",
            &stmt, err);
        if (st != ATLAS_OK) {
            return st;
        }
        if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK ||
            sqlite3_bind_int64(stmt, 2, keep) != SQLITE_OK) {
            atlas_db_finish(db, stmt);
            return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind generation prune");
        }
        int64_t victim = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            victim = sqlite3_column_int64(stmt, 0);
        }
        atlas_db_finish(db, stmt);
        if (victim == 0) {
            return ATLAS_OK;
        }
        st = atlas_db_sem_generation_delete(db, victim, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
}

atlas_status atlas_db_sem_forget_repo(atlas_db *db, int64_t repo_id, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st =
        atlas_db_prepare(db, "DELETE FROM sem_current WHERE repo_id = ?1;", &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    st = atlas_db_step_done(db, stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    stmt = NULL;
    st = atlas_db_prepare(db, "UPDATE sem_generations SET repo_id = 0 WHERE repo_id = ?1;", &stmt,
                          err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    return atlas_db_step_done(db, stmt, err);
}

/* --- writing a generation's contents ---------------------------------------- */

atlas_status atlas_db_sem_compdb_add(atlas_db *db, int64_t generation_id, const char *path_text,
                                     const char *digest, int64_t entries, int64_t dropped,
                                     int64_t *id_out, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "INSERT INTO sem_compdbs(generation_id, path_text, digest, entries, entries_dropped)"
        " VALUES(?1,?2,?3,?4,?5);",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, generation_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 4, entries) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 5, dropped) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind compile database");
    }
    st = bind_text(db, stmt, 2, path_text, err);
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 3, digest, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    st = atlas_db_step_done(db, stmt, err);
    if (st == ATLAS_OK && id_out != NULL) {
        *id_out = sqlite3_last_insert_rowid(db->h);
    }
    return st;
}

atlas_status atlas_db_sem_unit_add(atlas_db *db, const atlas_sem_unit_row *row, int64_t *id_out,
                                   atlas_err *err) {
    if (row == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "semantic unit: bad request");
    }
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "INSERT INTO sem_units(generation_id, source_text, compdb_id, config_digest,"
        "  input_digest, status, why, diagnostics_errors, symbols, edges, duration_ms, reused)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12)"
        " ON CONFLICT(generation_id, source_text, config_digest) DO UPDATE SET"
        "  status = excluded.status, why = excluded.why,"
        "  diagnostics_errors = excluded.diagnostics_errors, symbols = excluded.symbols,"
        "  edges = excluded.edges, duration_ms = excluded.duration_ms,"
        "  input_digest = excluded.input_digest, reused = excluded.reused;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, row->generation_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 3, row->compdb_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 8, row->diagnostics_errors) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 9, row->symbols) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 10, row->edges) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 11, row->duration_ms) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 12, row->reused ? 1 : 0) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind semantic unit");
    }
    st = bind_text(db, stmt, 2, row->source_text, err);
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 4, row->config_digest, err);
    }
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 5, row->input_digest, err);
    }
    if (st == ATLAS_OK) {
        /* UNKNOWN is not in the CHECK, so a status nobody set becomes FAILED
         * rather than being written as a runnable-looking value. */
        atlas_sem_tu_status s = row->status;
        if (s == ATLAS_SEM_TU_UNKNOWN) {
            s = ATLAS_SEM_TU_FAILED;
        }
        st = bind_text(db, stmt, 6, atlas_sem_tu_status_name(s), err);
    }
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 7, atlas_sem_why_is_known(row->why) ? row->why : "", err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    st = atlas_db_step_done(db, stmt, err);
    if (st != ATLAS_OK || id_out == NULL) {
        return st;
    }
    /* Looked up rather than taken from `sqlite3_last_insert_rowid`.
     *
     * This is an upsert, and on the DO UPDATE branch no row is inserted — so
     * `last_insert_rowid` would return whatever this connection inserted last,
     * which is some other table's row. A9.2.4 threads this id into
     * `atlas_db_sem_copy_unit`, where a wrong one would attach a unit's carried
     * edges to a different unit entirely: silently, and only on the replay path
     * a crash produces. Asked of the table's own unique key instead, which is
     * the only value that is right in both branches. */
    stmt = NULL;
    atlas_status lst = atlas_db_prepare(db,
                                        "SELECT id FROM sem_units WHERE generation_id = ?1"
                                        "  AND source_text = ?2 AND config_digest = ?3;",
                                        &stmt, err);
    if (lst != ATLAS_OK) {
        return lst;
    }
    if (sqlite3_bind_int64(stmt, 1, row->generation_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the generation");
    }
    lst = bind_text(db, stmt, 2, row->source_text, err);
    if (lst == ATLAS_OK) {
        lst = bind_text(db, stmt, 3, row->config_digest != NULL ? row->config_digest : "", err);
    }
    if (lst != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return lst;
    }
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        *id_out = sqlite3_column_int64(stmt, 0);
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_sem_symbol_add(atlas_db *db, int64_t generation_id,
                                     const atlas_sem_fact *f, atlas_err *err) {
    if (f == NULL || f->usr == NULL || f->usr[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "semantic symbol: no identity");
    }
    /* Checked at the write point, not merely at the producer. A symbol row
     * always states PROVEN — the compiler found it — but the class is validated
     * rather than assumed, so an unrecognised value cannot be stored. */
    atlas_sem_symbol_kind kind = ATLAS_SEM_SYM_UNKNOWN;
    (void)atlas_sem_symbol_kind_parse(f->kind, &kind);
    atlas_sem_linkage link = ATLAS_SEM_LINK_UNKNOWN;
    (void)atlas_sem_linkage_parse(f->linkage, &link);

    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "INSERT INTO sem_symbols(generation_id, usr, name, kind, linkage, type_text, file_text,"
        "  line, col, end_line, is_definition, external, evidence)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,'PROVEN')"
        " ON CONFLICT(generation_id, usr, file_text, line, is_definition) DO NOTHING;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, generation_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 8, f->line) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 9, f->col) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 10, f->end_line) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 11, f->is_definition ? 1 : 0) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 12, f->external ? 1 : 0) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind semantic symbol");
    }
    st = bind_text(db, stmt, 2, f->usr, err);
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 3, f->name, err);
    }
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 4, atlas_sem_symbol_kind_name(kind), err);
    }
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 5, atlas_sem_linkage_name(link), err);
    }
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 6, f->type_text, err);
    }
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 7, f->file, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_sem_edge_add(atlas_db *db, int64_t generation_id, int64_t unit_id,
                                   const atlas_sem_fact *f, atlas_err *err) {
    if (f == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "semantic edge: bad request");
    }
    atlas_sem_edge_kind kind = ATLAS_SEM_EDGE_UNKNOWN;
    if (!atlas_sem_edge_kind_parse(f->kind, &kind) || kind == ATLAS_SEM_EDGE_UNKNOWN) {
        /* An edge kind Atlas does not recognise is not stored. The CHECK would
         * refuse it anyway; refusing here produces the better message and keeps
         * the guarantee at the write point. */
        return ATLAS_OK;
    }
    atlas_sem_evidence ev = ATLAS_SEM_EV_UNKNOWN;
    (void)atlas_sem_evidence_parse(f->evidence, &ev);
    /* The cap is applied here as well as in the extractor. Two places on
     * purpose: the extractor produces the better locality, this is the
     * guarantee — A4's shape for the actor restriction. A MAY_CALL edge cannot
     * be stored as PROVEN by any path. */
    ev = atlas_sem_evidence_weaker(ev, atlas_sem_edge_kind_max_evidence(kind));

    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "INSERT INTO sem_edges(generation_id, kind, src_usr, dst_usr, evidence, unit_id,"
        "  file_text, line, col, proto)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)"
        " ON CONFLICT(generation_id, kind, src_usr, dst_usr, file_text, line, col) DO NOTHING;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, generation_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 6, unit_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 8, f->line) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 9, f->col) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind semantic edge");
    }
    st = bind_text(db, stmt, 2, atlas_sem_edge_kind_name(kind), err);
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 3, f->src_usr, err);
    }
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 4, f->dst_usr, err);
    }
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 5, atlas_sem_evidence_name(ev), err);
    }
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 7, f->file, err);
    }
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 10, f->detail, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_sem_include_add(atlas_db *db, int64_t generation_id,
                                      const atlas_sem_fact *f, atlas_err *err) {
    if (f == NULL || f->include_from == NULL || f->include_from[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "semantic include: no source file");
    }
    atlas_sem_evidence ev = ATLAS_SEM_EV_UNKNOWN;
    (void)atlas_sem_evidence_parse(f->evidence, &ev);

    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "INSERT INTO sem_includes(generation_id, from_text, to_text, spelling, line, evidence)"
        " VALUES(?1,?2,?3,?4,?5,?6)"
        " ON CONFLICT(generation_id, from_text, to_text, spelling, line) DO NOTHING;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, generation_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 5, f->line) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind semantic include");
    }
    st = bind_text(db, stmt, 2, f->include_from, err);
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 3, f->include_to, err);
    }
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 4, f->dst_name, err);
    }
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 6, atlas_sem_evidence_name(ev), err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

/* --- carrying a unit forward -------------------------------------------------- */

/* `to_unit_id` is bound as `?4` and is ignored by the statements that do not
 * name it — only the edge copy does, because only an edge belongs to a unit. */
static atlas_status copy_one(atlas_db *db, const char *sql, int64_t to_gen, int64_t from_gen,
                             const char *source_text, int64_t to_unit_id, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, sql, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, to_gen) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, from_gen) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind unit copy");
    }
    if (sqlite3_bind_parameter_count(stmt) >= 4 &&
        sqlite3_bind_int64(stmt, 4, to_unit_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the destination unit");
    }
    st = bind_text(db, stmt, 3, source_text, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_sem_copy_unit(atlas_db *db, int64_t from_generation, int64_t to_generation,
                                    int64_t to_unit_id, const char *source_text,
                                    const char *config_digest, int64_t *symbols_out,
                                    int64_t *edges_out, atlas_err *err) {
    (void)config_digest;
    /* Facts are carried by the unit that produced them. `sem_edges.unit_id`
     * names it directly; symbols and includes are reached through that unit's
     * edges and through the files it described, which is why the symbol copy
     * selects on the unit's own file set rather than on a unit column the table
     * does not have. The `INSERT OR IGNORE` is what makes copying two units
     * that share a header produce one row rather than a constraint failure. */
    atlas_status st = copy_one(
        db,
        "INSERT OR IGNORE INTO sem_edges(generation_id, kind, src_usr, dst_usr, evidence,"
        "  unit_id, file_text, line, col, proto, candidate_total)"
        /* `?4`, never `e.unit_id`: an edge belongs to the unit in *this*
         * generation that produced it. Carrying the ancestor's id across left
         * every generation referencing rows it did not contain, and made the
         * next pass's join find nothing once those rows were gone. */
        " SELECT ?1, e.kind, e.src_usr, e.dst_usr, e.evidence, ?4, e.file_text, e.line,"
        "  e.col, e.proto, e.candidate_total"
        " FROM sem_edges e JOIN sem_units u ON u.id = e.unit_id"
        " WHERE e.generation_id = ?2 AND u.generation_id = ?2 AND u.source_text = ?3;",
        to_generation, from_generation, source_text, to_unit_id, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (edges_out != NULL) {
        *edges_out += sqlite3_changes(db->h);
    }

    /* Symbols live in the unit's source and in every file it transitively
     * includes — the same closure `atlas_db_sem_unit_inputs` computes, and it
     * has to be the same one. A shallower walk here would carry an incomplete
     * set of symbols forward, so a reused unit would describe less than the
     * unit that produced it, and the difference would look like code that had
     * been deleted. */
    st = copy_one(db,
                  "WITH RECURSIVE reach(path, depth) AS ("
                  "  SELECT ?3, 0"
                  "  UNION"
                  "  SELECT i.to_text, r.depth + 1 FROM sem_includes i JOIN reach r"
                  "    ON i.from_text = r.path"
                  "   WHERE i.generation_id = ?2 AND i.to_text <> '' AND r.depth < 64)"
                  " INSERT OR IGNORE INTO sem_symbols(generation_id, usr, name, kind, linkage,"
                  "  type_text, file_text, line, col, end_line, is_definition, external, evidence)"
                  " SELECT ?1, s.usr, s.name, s.kind, s.linkage, s.type_text, s.file_text, s.line,"
                  "  s.col, s.end_line, s.is_definition, s.external, s.evidence"
                  " FROM sem_symbols s"
                  " WHERE s.generation_id = ?2"
                  "   AND (s.external = 1 OR s.file_text IN (SELECT path FROM reach));",
                  to_generation, from_generation, source_text, to_unit_id, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (symbols_out != NULL) {
        *symbols_out += sqlite3_changes(db->h);
    }

    /* The include rows for the whole closure, not just the unit's own
     * directives.
     *
     * Without this a carried-forward unit would arrive in the new generation
     * with a one-level include graph, and the *next* incremental pass would
     * compute its closure from that — a shallower set each time, until a header
     * edit stopped invalidating anything. The digest and the graph it is
     * computed from have to be carried together or neither is meaningful. */
    return copy_one(db,
                    "WITH RECURSIVE reach(path, depth) AS ("
                    "  SELECT ?3, 0"
                    "  UNION"
                    "  SELECT i.to_text, r.depth + 1 FROM sem_includes i JOIN reach r"
                    "    ON i.from_text = r.path"
                    "   WHERE i.generation_id = ?2 AND i.to_text <> '' AND r.depth < 64)"
                    " INSERT OR IGNORE INTO sem_includes(generation_id, from_text, to_text,"
                    "  spelling, line, evidence)"
                    " SELECT ?1, i.from_text, i.to_text, i.spelling, i.line, i.evidence"
                    " FROM sem_includes i"
                    " WHERE i.generation_id = ?2 AND i.from_text IN (SELECT path FROM reach);",
                    to_generation, from_generation, source_text, to_unit_id, err);
}

/* --- candidate targets for indirect calls ------------------------------------ */

atlas_status atlas_db_sem_attach_candidates(atlas_db *db, int64_t generation_id,
                                            int64_t max_per_site, int64_t *attached_out,
                                            atlas_err *err) {
    if (max_per_site <= 0) {
        max_per_site = ATLAS_SEM_MAX_INDIRECT_CANDIDATES;
    }

    /* First: how many candidates each prototype actually has. Recorded on every
     * MAY_CALL site *before* any are attached, so `candidate_total` reports the
     * true number even where the ceiling keeps fewer — A3's rule that a bound
     * which makes an ambiguity look smaller than it is is a bound that lies. */
    atlas_status st = exec_gen_scoped(
        db,
        "UPDATE sem_edges SET candidate_total = ("
        "  SELECT COUNT(DISTINCT a.dst_usr) FROM sem_edges a"
        "  WHERE a.generation_id = sem_edges.generation_id AND a.kind = 'ADDRESS_TAKEN'"
        "    AND a.proto <> '' AND a.proto = sem_edges.proto)"
        " WHERE generation_id = ?1 AND kind = 'MAY_CALL' AND proto <> '';",
        generation_id, err);
    if (st != ATLAS_OK) {
        return st;
    }

    /* Then the edges themselves. CANDIDATE, never PROVEN: `sem_edges`' CHECK
     * permits the class, and `atlas_sem_edge_kind_max_evidence` caps MAY_CALL
     * at exactly this — the two agree because the literal here is the only
     * value this statement can write. */
    sqlite3_stmt *stmt = NULL;
    st = atlas_db_prepare(
        db,
        "INSERT OR IGNORE INTO sem_edges(generation_id, kind, src_usr, dst_usr, evidence,"
        "  unit_id, file_text, line, col, proto, candidate_total)"
        " SELECT m.generation_id, 'MAY_CALL', m.src_usr, c.dst_usr, 'CANDIDATE', m.unit_id,"
        "        m.file_text, m.line, m.col, m.proto, m.candidate_total"
        " FROM sem_edges m"
        " JOIN (SELECT DISTINCT generation_id, proto, dst_usr FROM sem_edges"
        "        WHERE generation_id = ?1 AND kind = 'ADDRESS_TAKEN' AND proto <> '') c"
        "   ON c.generation_id = m.generation_id AND c.proto = m.proto"
        " WHERE m.generation_id = ?1 AND m.kind = 'MAY_CALL' AND m.dst_usr = '' AND m.proto <> ''"
        "   AND (SELECT COUNT(*) FROM sem_edges x WHERE x.generation_id = m.generation_id"
        "         AND x.kind = 'MAY_CALL' AND x.src_usr = m.src_usr AND x.file_text = m.file_text"
        "         AND x.line = m.line AND x.col = m.col AND x.dst_usr <> '') < ?2;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, generation_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, max_per_site) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind candidate attachment");
    }
    st = atlas_db_step_done(db, stmt, err);
    if (st == ATLAS_OK && attached_out != NULL) {
        *attached_out = sqlite3_changes(db->h);
    }
    return st;
}

/* --- bounded reads ------------------------------------------------------------ */

static void read_symbol(sqlite3_stmt *stmt, atlas_sem_symbol_row *row) {
    memset(row, 0, sizeof(*row));
    row->id = sqlite3_column_int64(stmt, 0);
    row->usr = col_text(stmt, 1);
    row->name = col_text(stmt, 2);
    row->kind = col_text(stmt, 3);
    row->linkage = col_text(stmt, 4);
    row->type_text = col_text(stmt, 5);
    row->file_text = col_text(stmt, 6);
    row->line = sqlite3_column_int64(stmt, 7);
    row->col = sqlite3_column_int64(stmt, 8);
    row->end_line = sqlite3_column_int64(stmt, 9);
    row->is_definition = sqlite3_column_int(stmt, 10) != 0;
    row->external = sqlite3_column_int(stmt, 11) != 0;
    row->evidence = col_text(stmt, 12);
}

#define SYM_COLUMNS                                                                            \
    "s.id, s.usr, s.name, s.kind, s.linkage, s.type_text, s.file_text, s.line, s.col,"         \
    " s.end_line, s.is_definition, s.external, s.evidence"

/* One driver for both symbol reads: identical bounding, identical truncation
 * reporting, identical deterministic order. Two copies would answer differently
 * the first time somebody fixed a bug in one of them — A3's argument for a
 * single walk. */
static atlas_status symbols_query(atlas_db *db, const char *sql, int64_t generation_id,
                                  const char *a, const char *b, int64_t limit,
                                  atlas_sem_symbol_cb cb, void *ud, int64_t *total_out,
                                  bool *truncated_out, atlas_err *err) {
    if (limit <= 0 || limit > ATLAS_SEM_MAX_ROWS) {
        limit = ATLAS_SEM_MAX_ROWS;
    }
    if (total_out != NULL) {
        *total_out = 0;
    }
    if (truncated_out != NULL) {
        *truncated_out = false;
    }
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, sql, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    /* One more than asked for, so "there was more" is observed rather than
     * inferred from a full page. */
    if (sqlite3_bind_int64(stmt, 1, generation_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 4, limit + 1) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind symbol query");
    }
    st = bind_text(db, stmt, 2, a, err);
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 3, b, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }

    int64_t emitted = 0;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (emitted >= limit) {
            if (truncated_out != NULL) {
                *truncated_out = true;
            }
            break;
        }
        atlas_sem_symbol_row row;
        read_symbol(stmt, &row);
        if (cb != NULL) {
            st = cb(&row, ud, err);
            if (st != ATLAS_OK) {
                break;
            }
        }
        emitted++;
    }
    if (st == ATLAS_OK && rc != SQLITE_ROW && rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read semantic symbols");
    }
    atlas_db_finish(db, stmt);
    if (total_out != NULL) {
        *total_out = emitted;
    }
    return st;
}

atlas_status atlas_db_sem_symbols_by_name(atlas_db *db, int64_t generation_id, const char *name,
                                          const char *usr, const char *kind, int64_t limit,
                                          atlas_sem_symbol_cb cb, void *ud, int64_t *total_out,
                                          bool *truncated_out, atlas_err *err) {
    /* Two statements rather than one with an extra predicate, because
     * `atlas_db_prepare` caches by the SQL *pointer* and both must therefore be
     * distinct literals. They also want different plans: the name lookup drives
     * `idx_sem_symbols_name` and the USR lookup drives `idx_sem_symbols_usr`.
     *
     * Definitions first, then repository symbols before external ones, then by
     * file and line: a deterministic order that puts the answer somebody
     * usually wants at the top without hiding the rest. A name that resolves to
     * several symbols returns all of them — choosing would be inventing, which
     * is A3's rule about ambiguity and it holds here too. */
    if (usr != NULL && usr[0] != '\0') {
        return symbols_query(db,
                             "SELECT " SYM_COLUMNS " FROM sem_symbols s"
                             " WHERE s.generation_id = ?1 AND s.usr = ?2"
                             "   AND (?3 = '' OR s.kind = ?3)"
                             " ORDER BY s.is_definition DESC, s.external ASC, s.file_text, s.line"
                             " LIMIT ?4;",
                             generation_id, usr, kind != NULL ? kind : "", limit, cb, ud,
                             total_out, truncated_out, err);
    }
    return symbols_query(db,
                         "SELECT " SYM_COLUMNS " FROM sem_symbols s"
                         " WHERE s.generation_id = ?1 AND s.name = ?2"
                         "   AND (?3 = '' OR s.kind = ?3)"
                         " ORDER BY s.is_definition DESC, s.external ASC, s.file_text, s.line"
                         " LIMIT ?4;",
                         generation_id, name != NULL ? name : "", kind != NULL ? kind : "", limit,
                         cb, ud, total_out, truncated_out, err);
}

atlas_status atlas_db_sem_symbols_in_file(atlas_db *db, int64_t generation_id,
                                          const char *file_text, int64_t limit,
                                          atlas_sem_symbol_cb cb, void *ud, int64_t *total_out,
                                          bool *truncated_out, atlas_err *err) {
    return symbols_query(db,
                         "SELECT " SYM_COLUMNS " FROM sem_symbols s"
                         " WHERE s.generation_id = ?1 AND s.file_text = ?2 AND ?3 = ''"
                         " ORDER BY s.line, s.col LIMIT ?4;",
                         generation_id, file_text, "", limit, cb, ud, total_out, truncated_out,
                         err);
}

static void read_edge(sqlite3_stmt *stmt, atlas_sem_edge_row *row) {
    memset(row, 0, sizeof(*row));
    row->id = sqlite3_column_int64(stmt, 0);
    row->kind = col_text(stmt, 1);
    row->src_usr = col_text(stmt, 2);
    row->dst_usr = col_text(stmt, 3);
    row->evidence = col_text(stmt, 4);
    row->file_text = col_text(stmt, 5);
    row->line = sqlite3_column_int64(stmt, 6);
    row->col = sqlite3_column_int64(stmt, 7);
    row->proto = col_text(stmt, 8);
    row->candidate_total = sqlite3_column_int64(stmt, 9);
    row->peer_name = col_text(stmt, 10);
    row->peer_file = col_text(stmt, 11);
    row->peer_line = sqlite3_column_int64(stmt, 12);
}

atlas_status atlas_db_sem_edges_of(atlas_db *db, int64_t generation_id, const char *usr,
                                   bool inbound, const char *kind, bool calls_only, int64_t limit,
                                   atlas_sem_edge_cb cb, void *ud, int64_t *total_out,
                                   bool *truncated_out, atlas_err *err) {
    if (limit <= 0 || limit > ATLAS_SEM_MAX_ROWS) {
        limit = ATLAS_SEM_MAX_ROWS;
    }
    if (total_out != NULL) {
        *total_out = 0;
    }
    if (truncated_out != NULL) {
        *truncated_out = false;
    }

    /* Two statements rather than one with a swapped column, because
     * `atlas_db_prepare` caches by the SQL pointer and both must be distinct
     * literals. The peer join resolves the far end's display name and best
     * location — a definition when there is one — so a caller does not issue a
     * lookup per edge.
     *
     * The inbound query requires a non-empty `dst_usr` implicitly by matching
     * on it: an indirect call with no destination can never be an edge *into*
     * anything, and returning one would let an unknown target appear as a
     * caller. */
    static const char *const INBOUND =
        "SELECT e.id, e.kind, e.src_usr, e.dst_usr, e.evidence, e.file_text, e.line, e.col,"
        "       e.proto, e.candidate_total,"
        "       COALESCE(p.name,''), COALESCE(p.file_text,''), COALESCE(p.line,0)"
        " FROM sem_edges e"
        " LEFT JOIN sem_symbols p ON p.generation_id = e.generation_id AND p.usr = e.src_usr"
        "   AND p.id = (SELECT q.id FROM sem_symbols q WHERE q.generation_id = e.generation_id"
        "                AND q.usr = e.src_usr ORDER BY q.is_definition DESC, q.line LIMIT 1)"
        " WHERE e.generation_id = ?1 AND e.dst_usr = ?2 AND (?3 = '' OR e.kind = ?3)"
        "   AND (?5 = 0 OR e.kind IN ('CALLS','MAY_CALL'))"
        " ORDER BY e.evidence, e.file_text, e.line, e.col LIMIT ?4;";
    static const char *const OUTBOUND =
        "SELECT e.id, e.kind, e.src_usr, e.dst_usr, e.evidence, e.file_text, e.line, e.col,"
        "       e.proto, e.candidate_total,"
        "       COALESCE(p.name,''), COALESCE(p.file_text,''), COALESCE(p.line,0)"
        " FROM sem_edges e"
        " LEFT JOIN sem_symbols p ON p.generation_id = e.generation_id AND p.usr = e.dst_usr"
        "   AND p.id = (SELECT q.id FROM sem_symbols q WHERE q.generation_id = e.generation_id"
        "                AND q.usr = e.dst_usr ORDER BY q.is_definition DESC, q.line LIMIT 1)"
        " WHERE e.generation_id = ?1 AND e.src_usr = ?2 AND (?3 = '' OR e.kind = ?3)"
        "   AND (?5 = 0 OR e.kind IN ('CALLS','MAY_CALL'))"
        " ORDER BY e.evidence, e.file_text, e.line, e.col LIMIT ?4;";

    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, inbound ? INBOUND : OUTBOUND, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, generation_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 4, limit + 1) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 5, calls_only ? 1 : 0) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind edge query");
    }
    st = bind_text(db, stmt, 2, usr, err);
    if (st == ATLAS_OK) {
        /* A kind Atlas does not recognise selects nothing rather than being
         * passed through: the vocabulary is closed, and a filter naming
         * something outside it is a caller error, not a wildcard. */
        atlas_sem_edge_kind k = ATLAS_SEM_EDGE_UNKNOWN;
        bool named = kind != NULL && kind[0] != '\0';
        if (named && !atlas_sem_edge_kind_parse(kind, &k)) {
            atlas_db_finish(db, stmt);
            return atlas_err_set(err, ATLAS_ERR_USAGE, "unknown semantic edge kind");
        }
        st = bind_text(db, stmt, 3, named ? atlas_sem_edge_kind_name(k) : "", err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }

    int64_t emitted = 0;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (emitted >= limit) {
            if (truncated_out != NULL) {
                *truncated_out = true;
            }
            break;
        }
        atlas_sem_edge_row row;
        read_edge(stmt, &row);
        if (cb != NULL) {
            st = cb(&row, ud, err);
            if (st != ATLAS_OK) {
                break;
            }
        }
        emitted++;
    }
    if (st == ATLAS_OK && rc != SQLITE_ROW && rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read semantic edges");
    }
    atlas_db_finish(db, stmt);
    if (total_out != NULL) {
        *total_out = emitted;
    }
    return st;
}

atlas_status atlas_db_sem_includers_of(atlas_db *db, int64_t generation_id, const char *file_text,
                                       int64_t limit, atlas_sem_edge_cb cb, void *ud,
                                       int64_t *total_out, bool *truncated_out, atlas_err *err) {
    if (limit <= 0 || limit > ATLAS_SEM_MAX_ROWS) {
        limit = ATLAS_SEM_MAX_ROWS;
    }
    if (total_out != NULL) {
        *total_out = 0;
    }
    if (truncated_out != NULL) {
        *truncated_out = false;
    }
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "SELECT i.id, 'INCLUDES', i.from_text, i.to_text, i.evidence, i.from_text, i.line, 0,"
        "       '', 0, '', i.from_text, i.line"
        " FROM sem_includes i"
        " WHERE i.generation_id = ?1 AND i.to_text = ?2"
        " ORDER BY i.from_text, i.line LIMIT ?3;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, generation_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 3, limit + 1) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind include query");
    }
    st = bind_text(db, stmt, 2, file_text, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    int64_t emitted = 0;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (emitted >= limit) {
            if (truncated_out != NULL) {
                *truncated_out = true;
            }
            break;
        }
        atlas_sem_edge_row row;
        read_edge(stmt, &row);
        if (cb != NULL) {
            st = cb(&row, ud, err);
            if (st != ATLAS_OK) {
                break;
            }
        }
        emitted++;
    }
    if (st == ATLAS_OK && rc != SQLITE_ROW && rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read include relations");
    }
    atlas_db_finish(db, stmt);
    if (total_out != NULL) {
        *total_out = emitted;
    }
    return st;
}

atlas_status atlas_db_sem_failed_units(atlas_db *db, int64_t generation_id, int64_t limit,
                                       atlas_sem_unit_cb cb, void *ud, int64_t *total_out,
                                       bool *truncated_out, atlas_err *err) {
    if (limit <= 0 || limit > ATLAS_SEM_MAX_ROWS) {
        limit = ATLAS_SEM_MAX_ROWS;
    }
    if (total_out != NULL) {
        *total_out = 0;
    }
    if (truncated_out != NULL) {
        *truncated_out = false;
    }
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "SELECT u.source_text, u.status, u.why, u.diagnostics_errors FROM sem_units u"
        " WHERE u.generation_id = ?1 AND u.status <> 'COMPLETE'"
        " ORDER BY u.status, u.source_text LIMIT ?2;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, generation_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, limit + 1) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind unit report");
    }
    int64_t emitted = 0;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (emitted >= limit) {
            if (truncated_out != NULL) {
                *truncated_out = true;
            }
            break;
        }
        atlas_sem_unit_report row;
        row.source_text = col_text(stmt, 0);
        row.status = col_text(stmt, 1);
        row.why = col_text(stmt, 2);
        row.diagnostics_errors = sqlite3_column_int64(stmt, 3);
        if (cb != NULL) {
            st = cb(&row, ud, err);
            if (st != ATLAS_OK) {
                break;
            }
        }
        emitted++;
    }
    if (st == ATLAS_OK && rc != SQLITE_ROW && rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read semantic units");
    }
    atlas_db_finish(db, stmt);
    if (total_out != NULL) {
        *total_out = emitted;
    }
    return st;
}

atlas_status atlas_db_sem_unit_digest(atlas_db *db, int64_t generation_id, const char *source_text,
                                      const char *config_digest, atlas_buf *digest_out,
                                      bool *found, atlas_err *err) {
    *found = false;
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        /* A9.2.5. **`status = 'COMPLETE'` is the whole of the correctness here.**
         *
         * Without it a unit that FAILED — a parse child killed twice, a compiler
         * that produced no translation unit — is carried forward on a digest
         * match alone and rewritten as COMPLETE with the zero symbols and zero
         * edges it never produced. One unrelated edit anywhere in the tree moves
         * the source identity, the next pass carries the failure forward as a
         * success, and the generation then reports `tu_failed = 0`,
         * `scope_uncovered = 0` and complete coverage over a file Atlas has
         * never parsed.
         *
         * That was survivable while these counters only advised the scheduler.
         * A9.2.5 makes `units_complete` a gate on `ATLAS_SEM_VERDICT_ABSENT`, so
         * it became a path to a *proven absence* over an unread file, with every
         * field in the trust block saying the coverage was whole. A unit that did
         * not succeed is re-parsed; that is the only honest carry rule. */
        "SELECT u.input_digest FROM sem_units u"
        " WHERE u.generation_id = ?1 AND u.source_text = ?2 AND u.config_digest = ?3"
        "   AND u.status = 'COMPLETE';",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, generation_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind unit digest lookup");
    }
    st = bind_text(db, stmt, 2, source_text, err);
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 3, config_digest, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *d = col_text(stmt, 0);
        if (d[0] != '\0') {
            st = atlas_buf_set_str(digest_out, d, err);
            *found = st == ATLAS_OK;
        }
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_sem_unit_inputs(atlas_db *db, int64_t generation_id, const char *source_text,
                                      atlas_buf *paths_out, atlas_err *err) {
    /* The unit's own source, plus the **transitive closure** of everything it
     * includes, in sorted order.
     *
     * Transitive is the whole point and the shallow version of this query was a
     * real defect: a header four levels down is exactly the file whose edit an
     * incremental pass must notice, and a two-level walk would leave it out of
     * the digest, carry the unit forward unchanged and report it COMPLETE.
     *
     * Sorted because the digest computed over this list must not depend on
     * SQLite's row order — the whole point of the digest is that identical
     * inputs produce an identical value on a later run. `UNION` rather than
     * `UNION ALL` terminates the recursion on a cyclic include graph, which C
     * produces routinely through include guards. */
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "WITH RECURSIVE reach(path, depth) AS ("
        "  SELECT ?2, 0"
        "  UNION"
        "  SELECT i.to_text, r.depth + 1 FROM sem_includes i JOIN reach r"
        "    ON i.from_text = r.path"
        "   WHERE i.generation_id = ?1 AND i.to_text <> '' AND r.depth < ?3)"
        " SELECT DISTINCT path FROM reach WHERE path <> ?2 ORDER BY path;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, generation_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 3, ATLAS_SEM_MAX_INCLUDE_DEPTH) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind unit inputs");
    }
    st = bind_text(db, stmt, 2, source_text, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    int rc;
    while (st == ATLAS_OK && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *p = col_text(stmt, 0);
        st = atlas_buf_append(paths_out, p, strlen(p) + 1, err);
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_sem_units_all(atlas_db *db, int64_t generation_id,
                                    atlas_sem_unit_key_cb cb, void *ud, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    /* Ordered, so the sealing pass visits units in a reproducible order and two
     * runs over one state produce identical generations. */
    atlas_status st = atlas_db_prepare(db,
                                       /* A9.2.5. Only a COMPLETE unit is sealed
                                        * with a reusable digest, for the reason
                                        * `atlas_db_sem_unit_digest` filters on
                                        * the same column: a digest sealed onto a
                                        * failed unit is an invitation for the
                                        * next pass to carry the failure forward
                                        * as a success. */
                                       "SELECT u.source_text, u.config_digest FROM sem_units u"
                                       " WHERE u.generation_id = ?1 AND u.status = 'COMPLETE'"
                                       " ORDER BY u.source_text, u.config_digest;",
                                       &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, generation_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind unit enumeration");
    }
    int rc;
    while (st == ATLAS_OK && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        atlas_sem_unit_key key;
        key.source_text = col_text(stmt, 0);
        key.config_digest = col_text(stmt, 1);
        if (cb != NULL) {
            st = cb(&key, ud, err);
        }
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_sem_unit_set_digest(atlas_db *db, int64_t generation_id,
                                          const char *source_text, const char *config_digest,
                                          const char *digest, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "UPDATE sem_units SET input_digest = ?4"
        " WHERE generation_id = ?1 AND source_text = ?2 AND config_digest = ?3;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, generation_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind unit digest update");
    }
    st = bind_text(db, stmt, 2, source_text, err);
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 3, config_digest, err);
    }
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 4, digest, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

/* One bounded count over a generation-scoped index. */
static atlas_status count_scoped(atlas_db *db, const char *sql, int64_t generation_id,
                                 int64_t *out, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, sql, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, generation_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind a generation id");
    }
    *out = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        *out = sqlite3_column_int64(stmt, 0);
        st = ATLAS_OK;
    } else {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot count a generation");
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_sem_generation_counts(atlas_db *db, int64_t generation_id,
                                            int64_t *symbols_out, int64_t *edges_out,
                                            int64_t *includes_out, atlas_err *err) {
    /* Three string literals rather than one built string: `atlas_db_prepare`
     * caches on the SQL pointer, so every call site passes a literal. */
    atlas_status st =
        count_scoped(db, "SELECT count(*) FROM sem_symbols WHERE generation_id = ?1;",
                     generation_id, symbols_out, err);
    if (st == ATLAS_OK) {
        st = count_scoped(db, "SELECT count(*) FROM sem_edges WHERE generation_id = ?1;",
                          generation_id, edges_out, err);
    }
    if (st == ATLAS_OK) {
        st = count_scoped(db, "SELECT count(*) FROM sem_includes WHERE generation_id = ?1;",
                          generation_id, includes_out, err);
    }
    return st;
}

/* --- A9.2.3: the durable build description ----------------------------------
 *
 * The row is the operator's statement about a repository, and its absence is the
 * default: no row means this daemon never runs a compiler for that repository,
 * which is what keeps A8-CI's rule true after repository changes became a
 * rebuild trigger. Nothing here creates a row implicitly.
 */

atlas_status atlas_db_sem_config_get(atlas_db *db, int64_t repo_id, atlas_sem_config *out,
                                     atlas_err *err) {
    if (out == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "semantic configuration: bad request");
    }
    atlas_sem_config_init(out);
    out->repo_id = repo_id;
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db,
                                       "SELECT repo_identity_hash, auto_rebuild, compdbs,"
                                       "  test_roots, configured_at, fail_count, fail_identity,"
                                       "  fail_reason, fail_at, auto_intent, auto_intent_by,"
                                       "  discovery_mode, excludes, vendor_roots,"
                                       "  discovery_state, discovered_at, discovery_limit"
                                       " FROM sem_repo_config WHERE repo_id = ?1;",
                                       &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out->present = true;
        copy_field(out->repo_identity_hash, sizeof out->repo_identity_hash, col_text(stmt, 0));
        /* A9.2.4: **never the stored column.** It is a cache written by
         * `sem-config` under the compiled-in default, and migration 19 leaves it
         * at 0 for every row whose intent it could not recover — so a reader
         * that trusted it would disagree with every surface for exactly the
         * migrated rows. The intent is the authority; the plan recomputes the
         * effective answer against the live root-owned policy on every read,
         * which is A6's rule about never storing a derived answer. */
        (void)sqlite3_column_int64(stmt, 1);
        st = atlas_buf_set_str(&out->compdbs, col_text(stmt, 2), err);
        if (st == ATLAS_OK) {
            st = atlas_buf_set_str(&out->test_roots, col_text(stmt, 3), err);
        }
        copy_field(out->configured_at, sizeof out->configured_at, col_text(stmt, 4));
        out->fail_count = sqlite3_column_int64(stmt, 5);
        copy_field(out->fail_identity, sizeof out->fail_identity, col_text(stmt, 6));
        copy_field(out->fail_reason, sizeof out->fail_reason, col_text(stmt, 7));
        copy_field(out->fail_at, sizeof out->fail_at, col_text(stmt, 8));
        /* A9.2.4. An unrecognised stored value leaves the field at its zero,
         * which is the safe reading for all three: UNSET intent, DEFAULT
         * provenance, AUTOMATIC discovery. The CHECKs make an unrecognised value
         * impossible through Atlas; this is what happens if somebody edits the
         * database with `sqlite3`, and refusing to enable on a value Atlas does
         * not understand is the correct answer to that. */
        (void)atlas_sem_auto_intent_parse(col_text(stmt, 9), &out->auto_intent);
        out->auto_rebuild = atlas_sem_auto_effective(out->auto_intent, ATLAS_SEM_AUTO_DEFAULT);
        (void)atlas_sem_intent_source_parse(col_text(stmt, 10), &out->auto_intent_by);
        (void)atlas_sem_discovery_mode_parse(col_text(stmt, 11), &out->discovery_mode);
        if (st == ATLAS_OK) {
            st = atlas_buf_set_str(&out->excludes, col_text(stmt, 12), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_set_str(&out->vendor_roots, col_text(stmt, 13), err);
        }
        (void)atlas_sem_discovery_parse(col_text(stmt, 14), &out->discovery_state);
        copy_field(out->discovered_at, sizeof out->discovered_at, col_text(stmt, 15));
        copy_field(out->discovery_limit, sizeof out->discovery_limit, col_text(stmt, 16));
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_sem_config_set(atlas_db *db, const atlas_sem_config *c, atlas_err *err) {
    if (c == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "semantic configuration: bad request");
    }
    /* The retry-governor columns are deliberately absent from the update list.
     * Configuring a repository is not a claim that a previous failure has been
     * resolved, and clearing the record here would let one command turn a
     * deterministic failure back into a spin. What legitimately makes the next
     * attempt eligible is that changing the description moves the source
     * identity, which the governor compares against. */
    sqlite3_stmt *stmt = NULL;
    atlas_status st =
        atlas_db_prepare(db,
                         "INSERT INTO sem_repo_config(repo_id, repo_identity_hash, auto_rebuild,"
                         "  compdbs, test_roots, configured_at,"
                         "  auto_intent, auto_intent_by, discovery_mode, excludes, vendor_roots)"
                         " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)"
                         " ON CONFLICT(repo_id) DO UPDATE SET"
                         "  repo_identity_hash = excluded.repo_identity_hash,"
                         "  auto_rebuild = excluded.auto_rebuild,"
                         "  compdbs = excluded.compdbs,"
                         "  test_roots = excluded.test_roots,"
                         "  configured_at = excluded.configured_at,"
                         "  auto_intent = excluded.auto_intent,"
                         "  auto_intent_by = excluded.auto_intent_by,"
                         "  discovery_mode = excluded.discovery_mode,"
                         "  excludes = excluded.excludes,"
                         "  vendor_roots = excluded.vendor_roots;",
                         &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof now);
    if (sqlite3_bind_int64(stmt, 1, c->repo_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 3, c->auto_rebuild ? 1 : 0) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind semantic configuration");
    }
    st = bind_text(db, stmt, 2, c->repo_identity_hash, err);
    if (st == ATLAS_OK) {
        st = bind_buf(db, stmt, 4, &c->compdbs, err);
    }
    if (st == ATLAS_OK) {
        st = bind_buf(db, stmt, 5, &c->test_roots, err);
    }
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 6, now, err);
    }
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 7, atlas_sem_auto_intent_name(c->auto_intent), err);
    }
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 8, atlas_sem_intent_source_name(c->auto_intent_by), err);
    }
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 9, atlas_sem_discovery_mode_name(c->discovery_mode), err);
    }
    if (st == ATLAS_OK) {
        st = bind_buf(db, stmt, 10, &c->excludes, err);
    }
    if (st == ATLAS_OK) {
        st = bind_buf(db, stmt, 11, &c->vendor_roots, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

/* --- A9.2.4: the discovered candidates ---------------------------------------
 *
 * Rewritten whole by each discovery pass rather than merged into, because the
 * table is a *snapshot of a search*, not an accumulation of sightings. A row
 * left behind from a previous walk would be a candidate Atlas is no longer
 * asserting anything about, sitting beside ones it is — and nothing in the row
 * would say which kind it was. Delete-then-insert in one transaction is the
 * honest shape, and the caller owns that transaction. */
atlas_status atlas_db_sem_inputs_replace(atlas_db *db, int64_t repo_id,
                                         const struct atlas_sem_input *inputs, size_t count,
                                         const char *discovered_at, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st =
        atlas_db_prepare(db, "DELETE FROM sem_build_inputs WHERE repo_id = ?1;", &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    st = atlas_db_step_done(db, stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }

    for (size_t i = 0; i < count; i++) {
        const atlas_sem_input *in = &inputs[i];
        stmt = NULL;
        st = atlas_db_prepare(db,
                              "INSERT INTO sem_build_inputs(repo_id, path_text, origin, accepted,"
                              "  reject_reason, digest, unit_count, discovered_at)"
                              " VALUES(?1,?2,?3,?4,?5,?6,?7,?8);",
                              &stmt, err);
        if (st != ATLAS_OK) {
            return st;
        }
        if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK ||
            sqlite3_bind_int64(stmt, 4, in->accepted ? 1 : 0) != SQLITE_OK ||
            sqlite3_bind_int64(stmt, 7, in->unit_count) != SQLITE_OK) {
            atlas_db_finish(db, stmt);
            return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind a build input");
        }
        st = bind_text(db, stmt, 2, in->path, err);
        if (st == ATLAS_OK) {
            st = bind_text(db, stmt, 3, atlas_sem_input_origin_name(in->origin), err);
        }
        if (st == ATLAS_OK) {
            /* Interned rather than stored as the caller gave it: a reason that
             * arrived over a socket is a *matching* string, not Atlas' string,
             * and a column an operator reads must hold bytes Atlas owns. An
             * unrecognised value becomes empty rather than being stored. */
            const char *reason = atlas_sem_reject_intern(in->reject_reason);
            st = bind_text(db, stmt, 5, reason != NULL ? reason : "", err);
        }
        if (st == ATLAS_OK) {
            st = bind_text(db, stmt, 6, in->digest, err);
        }
        if (st == ATLAS_OK) {
            st = bind_text(db, stmt, 8, discovered_at != NULL ? discovered_at : "", err);
        }
        if (st != ATLAS_OK) {
            atlas_db_finish(db, stmt);
            return st;
        }
        st = atlas_db_step_done(db, stmt, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    return ATLAS_OK;
}

/* Records the verdict of one discovery pass.
 *
 * Upserts, because a repository with no build description still has a discovery
 * result — that is the whole point of the season: a repository nobody configured
 * is one Atlas looks at, and what it found has to be recorded somewhere. The row
 * this creates expresses no intent (`auto_intent` keeps its 'UNSET' default), so
 * creating it authorises nothing. */
atlas_status atlas_db_sem_discovery_set(atlas_db *db, int64_t repo_id,
                                        const char *repo_identity_hash, const char *state,
                                        const char *discovered_at, const char *limit_detail,
                                        atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st =
        atlas_db_prepare(db,
                         "INSERT INTO sem_repo_config(repo_id, repo_identity_hash,"
                         "  discovery_state, discovered_at, discovery_limit)"
                         " VALUES(?1,?2,?3,?4,?5)"
                         " ON CONFLICT(repo_id) DO UPDATE SET"
                         "  discovery_state = excluded.discovery_state,"
                         "  discovered_at = excluded.discovered_at,"
                         "  discovery_limit = excluded.discovery_limit;",
                         &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    st = bind_text(db, stmt, 2, repo_identity_hash != NULL ? repo_identity_hash : "", err);
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 3, state, err);
    }
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 4, discovered_at != NULL ? discovered_at : "", err);
    }
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 5, limit_detail != NULL ? limit_detail : "", err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_sem_inputs_get(atlas_db *db, int64_t repo_id, struct atlas_sem_input *out,
                                     size_t max, size_t *count_out, atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db,
                                       "SELECT path_text, origin, accepted, reject_reason,"
                                       "  digest, unit_count"
                                       " FROM sem_build_inputs WHERE repo_id = ?1"
                                       " ORDER BY path_text;",
                                       &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    size_t n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && n < max) {
        atlas_sem_input *in = &out[n];
        memset(in, 0, sizeof(*in));
        copy_field(in->path, sizeof in->path, col_text(stmt, 0));
        (void)atlas_sem_input_origin_parse(col_text(stmt, 1), &in->origin);
        in->accepted = sqlite3_column_int64(stmt, 2) != 0;
        copy_field(in->reject_reason, sizeof in->reject_reason, col_text(stmt, 3));
        copy_field(in->digest, sizeof in->digest, col_text(stmt, 4));
        in->unit_count = sqlite3_column_int64(stmt, 5);
        n++;
    }
    atlas_db_finish(db, stmt);
    if (count_out != NULL) {
        *count_out = n;
    }
    return ATLAS_OK;
}

atlas_status atlas_db_sem_inputs_forget(atlas_db *db, int64_t repo_id, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st =
        atlas_db_prepare(db, "DELETE FROM sem_build_inputs WHERE repo_id = ?1;", &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_sem_config_record_attempt(atlas_db *db, int64_t repo_id,
                                                const char *source_identity, bool ok,
                                                const char *why, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    /* Success clears the record entirely: a repository that has just built is
     * not one carrying a failure, and leaving a count behind would make the
     * governor's "has the identity moved?" question compare against an identity
     * that no longer describes a failure. */
    atlas_status st =
        atlas_db_prepare(db,
                         ok ? "UPDATE sem_repo_config SET fail_count = 0, fail_identity = '',"
                              "  fail_reason = '', fail_at = '' WHERE repo_id = ?1;"
                            : "UPDATE sem_repo_config SET fail_count = fail_count + 1,"
                              "  fail_identity = ?2, fail_reason = ?3, fail_at = ?4"
                              " WHERE repo_id = ?1;",
                         &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    if (!ok) {
        char now[ATLAS_TS_MAX];
        atlas_now_iso8601(now, sizeof now);
        st = bind_text(db, stmt, 2, source_identity, err);
        if (st == ATLAS_OK) {
            /* A fixed Atlas string. Never compiler output and never an error
             * message assembled from repository bytes. */
            st = bind_text(db, stmt, 3, why, err);
        }
        if (st == ATLAS_OK) {
            st = bind_text(db, stmt, 4, now, err);
        }
        if (st != ATLAS_OK) {
            atlas_db_finish(db, stmt);
            return st;
        }
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_sem_config_repos(atlas_db *db, int64_t *out, size_t max, size_t *count_out,
                                       bool *truncated_out, atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    if (truncated_out != NULL) {
        *truncated_out = false;
    }
    if (out == NULL || max == 0) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "semantic configuration: bad request");
    }
    sqlite3_stmt *stmt = NULL;
    /* A9.2.4: every registered repository, not only the configured ones. See the
     * declaration — an absent build description now means "nobody has said
     * anything", which the root-owned default resolves, so a repository with no
     * row has to be considered before it can be held or built. */
    atlas_status st = atlas_db_prepare(
        db, "SELECT id FROM repositories WHERE id > 0 ORDER BY id;", &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    size_t n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (n >= max) {
            /* Reported, never silently shortened: a repository dropped from a
             * scheduling sweep is one that never rebuilds, and nothing in a
             * truncated list would say so. */
            if (truncated_out != NULL) {
                *truncated_out = true;
            }
            break;
        }
        out[n++] = sqlite3_column_int64(stmt, 0);
    }
    atlas_db_finish(db, stmt);
    if (count_out != NULL) {
        *count_out = n;
    }
    return ATLAS_OK;
}

atlas_status atlas_db_sem_config_forget_repo(atlas_db *db, int64_t repo_id, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st =
        atlas_db_prepare(db, "DELETE FROM sem_repo_config WHERE repo_id = ?1;", &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    return atlas_db_step_done(db, stmt, err);
}

/* --- A9.2.3: the coverage manifest ------------------------------------------ */

atlas_status atlas_db_sem_scope_counts(atlas_db *db, int64_t repo_id, int64_t generation_id,
                                       int64_t *candidates_out, int64_t *covered_out,
                                       atlas_err *err) {
    if (candidates_out != NULL) {
        *candidates_out = 0;
    }
    if (covered_out != NULL) {
        *covered_out = 0;
    }
    /* The denominator is the *file index's* enumeration of the tree, never the
     * compilation database's own contents. `language` is the file index's own
     * classification, so nothing here hard-codes an extension and a repository
     * in another language is described by the same query. */
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db,
                                       "SELECT COUNT(*) FROM files"
                                       " WHERE repo_id = ?1 AND deleted = 0"
                                       "   AND file_type = 'regular' AND language = 'c';",
                                       &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    if (sqlite3_step(stmt) == SQLITE_ROW && candidates_out != NULL) {
        *candidates_out = sqlite3_column_int64(stmt, 0);
    }
    atlas_db_finish(db, stmt);

    /* Distinct sources, because one file compiled under two configurations is
     * two translation units and one covered source. Counting units here would
     * make a repository with several build configurations look like one whose
     * coverage exceeded its own tree. */
    stmt = NULL;
    st = atlas_db_prepare(db,
                          "SELECT COUNT(DISTINCT u.source_text) FROM sem_units u"
                          " JOIN files f ON f.repo_id = ?1 AND f.deleted = 0"
                          "   AND f.file_type = 'regular' AND f.language = 'c'"
                          "   AND f.path_text = u.source_text"
                          " WHERE u.generation_id = ?2;",
                          &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, generation_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the scope query");
    }
    if (sqlite3_step(stmt) == SQLITE_ROW && covered_out != NULL) {
        *covered_out = sqlite3_column_int64(stmt, 0);
    }
    atlas_db_finish(db, stmt);
    return ATLAS_OK;
}

/* A9.2.4. Candidate sources under an operator-declared vendor prefix.
 *
 * Counted in C rather than in SQL because the prefix rule is a path-component
 * match, which `LIKE` cannot express without matching `vendorish/` as well as
 * `vendor/` — and a production source misclassified as somebody else's code is
 * wrong in the one direction that matters, exactly as it is for test roots.
 * `atlas_sem_path_under_prefix` is the single implementation of that rule.
 *
 * Runs once, at publication, over the same candidate set the denominator counts. */
atlas_status atlas_db_sem_scope_vendor_count(atlas_db *db, int64_t repo_id,
                                             const char *packed_vendor_roots, int64_t *out,
                                             atlas_err *err) {
    if (out != NULL) {
        *out = 0;
    }
    if (packed_vendor_roots == NULL || packed_vendor_roots[0] == '\0') {
        /* No declared vendor prefix is "Atlas does not know of any", never
         * "there are none". Zero excluded is the honest count either way; what
         * differs is that nothing was classified, and the manifest says that by
         * carrying the operator's list rather than by a flag here. */
        return ATLAS_OK;
    }
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db,
                                       "SELECT path_text FROM files"
                                       " WHERE repo_id = ?1 AND deleted = 0"
                                       "   AND file_type = 'regular' AND language = 'c';",
                                       &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    int64_t n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (atlas_sem_path_under_prefix(packed_vendor_roots, col_text(stmt, 0))) {
            n++;
        }
    }
    atlas_db_finish(db, stmt);
    if (out != NULL) {
        *out = n;
    }
    return ATLAS_OK;
}

atlas_status atlas_db_sem_scope_test_split(atlas_db *db, int64_t generation_id,
                                           const char *packed_test_roots, int64_t *test_out,
                                           int64_t *production_out, bool *known_out,
                                           atlas_err *err) {
    if (test_out != NULL) {
        *test_out = 0;
    }
    if (production_out != NULL) {
        *production_out = 0;
    }
    if (known_out != NULL) {
        *known_out = false;
    }
    if (packed_test_roots == NULL || packed_test_roots[0] == '\0') {
        /* No declared roots. Both counts stay zero and `known` stays false,
         * which is "Atlas does not know which sources are tests" — a different
         * statement from "there are no test units", and the one that makes a
         * production-scope absence unanswerable rather than wrong. */
        return ATLAS_OK;
    }
    /* Classified in C rather than in SQL, because the match must end on a path
     * component boundary and `LIKE 'tests%'` would classify `tests_helper.c` as
     * a test. */
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db, "SELECT DISTINCT source_text FROM sem_units WHERE generation_id = ?1;", &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, generation_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind generation id");
    }
    int64_t t = 0;
    int64_t p = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (atlas_sem_path_is_test(packed_test_roots, col_text(stmt, 0))) {
            t++;
        } else {
            p++;
        }
    }
    atlas_db_finish(db, stmt);
    if (test_out != NULL) {
        *test_out = t;
    }
    if (production_out != NULL) {
        *production_out = p;
    }
    if (known_out != NULL) {
        *known_out = true;
    }
    return ATLAS_OK;
}

atlas_status atlas_db_sem_scope_set(atlas_db *db, int64_t generation_id,
                                    const atlas_sem_generation *m, atlas_err *err) {
    if (m == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "semantic coverage manifest: bad request");
    }
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db,
                                       "UPDATE sem_generations SET scope_discovery = ?2,"
                                       "  scope_candidates = ?3, scope_covered = ?4,"
                                       "  scope_uncovered = ?5, tu_test = ?6, tu_production = ?7,"
                                       "  test_scope_known = ?8, discovery = ?9,"
                                       "  input_count = ?10, scope_excluded = ?11"
                                       " WHERE id = ?1;",
                                       &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, generation_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 3, m->scope_candidates) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 4, m->scope_covered) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 5, m->scope_uncovered) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 6, m->tu_test) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 7, m->tu_production) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 8, m->test_scope_known ? 1 : 0) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 10, m->input_count) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 11, m->scope_excluded) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the coverage manifest");
    }
    st = bind_text(db, stmt, 2, atlas_sem_scope_discovery_name(m->scope_discovery), err);
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 9, atlas_sem_discovery_name(m->discovery), err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_sem_source_content_digest(atlas_db *db, int64_t repo_id, char out[65],
                                                atlas_err *err) {
    out[0] = '\0';
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db,
                                       "SELECT path_text, content_hash FROM files"
                                       " WHERE repo_id = ?1 AND deleted = 0"
                                       "   AND file_type = 'regular'"
                                       "   AND language IN ('c','c-header')"
                                       " ORDER BY path_text;",
                                       &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    atlas_sha256 h;
    atlas_sha256_init(&h);
    static const char DOMAIN[] = "atlas.sem.source-content.v1";
    unsigned char len[8];
    /* Length-prefixed, for A4's reason: with any single-byte delimiter, two
     * different (path, hash) lists could encode identically, and this value
     * decides whether a repository is rebuilt. */
    const char *first = DOMAIN;
    for (int i = 0; i < 8; i++) {
        len[i] = (unsigned char)((strlen(first) >> (8 * (7 - i))) & 0xffu);
    }
    atlas_sha256_update(&h, len, sizeof len);
    atlas_sha256_update(&h, first, strlen(first));

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *path = col_text(stmt, 0);
        const char *hash = col_text(stmt, 1);
        /* A file whose content hash the index does not hold contributes a fixed
         * marker, not nothing. Skipping it would make a file Atlas could not
         * read compare equal to one that was never there — and the second is a
         * repository that has not changed, while the first is one Atlas cannot
         * currently describe. */
        const char *parts[2] = {path, hash[0] != '\0' ? hash : "\x01unknown-content"};
        for (int p = 0; p < 2; p++) {
            size_t n = strlen(parts[p]);
            for (int i = 0; i < 8; i++) {
                len[i] = (unsigned char)((n >> (8 * (7 - i))) & 0xffu);
            }
            atlas_sha256_update(&h, len, sizeof len);
            atlas_sha256_update(&h, parts[p], n);
        }
    }
    atlas_db_finish(db, stmt);

    unsigned char digest[ATLAS_SHA256_DIGEST_LEN];
    atlas_sha256_final(&h, digest);
    atlas_hex_encode(digest, sizeof digest, out);
    out[ATLAS_SHA256_HEX_LEN] = '\0';
    return ATLAS_OK;
}

atlas_status atlas_db_sem_source_identity_set(atlas_db *db, int64_t generation_id,
                                              const char *identity, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db, "UPDATE sem_generations SET source_identity = ?2 WHERE id = ?1;", &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, generation_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind generation id");
    }
    st = bind_text(db, stmt, 2, identity, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_sem_reap_running(atlas_db *db, int64_t *reaped_out, atlas_err *err) {
    if (reaped_out != NULL) {
        *reaped_out = 0;
    }
    sqlite3_stmt *stmt = NULL;
    /* A generation left RUNNING is one whose pass died before it could publish
     * or fail. It is marked FAILED with a fixed Atlas reason rather than
     * deleted: "indexing this repository died four times today" is an
     * operational fact, and a table that only recorded outcomes it reached
     * could not state it.
     *
     * `sem_current` is untouched by construction — publication is the only
     * statement that repoints it, and this row was never published. The
     * last-known-good generation goes on being served throughout. */
    atlas_status st = atlas_db_prepare(db,
                                       "UPDATE sem_generations SET status = 'FAILED',"
                                       "  completed_at = ?1, failure_reason = ?2"
                                       " WHERE status = 'RUNNING';",
                                       &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof now);
    st = bind_text(db, stmt, 1, now, err);
    if (st == ATLAS_OK) {
        st = bind_text(db, stmt, 2, ATLAS_SEM_WHY_CHILD_FAILED, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    st = atlas_db_step_done(db, stmt, err);
    if (st == ATLAS_OK && reaped_out != NULL) {
        *reaped_out = sqlite3_changes(db->h);
    }
    return st;
}

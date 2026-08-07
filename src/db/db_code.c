/* Atlas - A3 storage: the structural code graph.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Typed operations over the migration-5 tables. sqlite3 stays in src/db as
 * everywhere else; extraction and resolution live in src/code.
 *
 * Three properties are enforced here rather than by convention:
 *
 *   - **Nothing here touches `evidence`.** `atlas_db_evidence_insert` still
 *     refuses everything but SOURCE and GIT. Structural facts carry their own
 *     resolution and provenance columns, so "how does Atlas know this?" and
 *     "what did a lexical scan guess?" stay different questions.
 *   - **Replacement is per file.** Every symbol, occurrence and relation records
 *     the `code_files` row that produced it, so reindexing one file is one
 *     delete by that column and then the inserts. Nothing walks the repository.
 *   - **Deletion is explicit.** `files` rows are tombstoned rather than removed,
 *     so a foreign key from `files` would fire only on `repo remove` — which is
 *     the one case it is not needed for. `atlas_db_code_files_to_remove` finds
 *     the orphans by a left join and the caller deletes them.
 *
 * Every query that feeds resolution is ordered by a stable key — raw path bytes,
 * then id — so the same repository resolves identically whatever order the
 * worker threads finished in.
 */
#include "db/db_internal.h"

#include <stdio.h>
#include <string.h>

#include "atlas/atlas.h"

/* --- small helpers ------------------------------------------------------- */

static atlas_status bind_i64(atlas_db *db, sqlite3_stmt *s, int i, int64_t v, atlas_err *err) {
    if (sqlite3_bind_int64(s, i, v) != SQLITE_OK) {
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind an integer");
    }
    return ATLAS_OK;
}

/* Binds an id, or SQL NULL when it is 0. Several columns here are nullable
 * references and 0 is the "absent" sentinel throughout Atlas. */
static atlas_status bind_id_opt(atlas_db *db, sqlite3_stmt *s, int i, int64_t v, atlas_err *err) {
    int rc = (v > 0) ? sqlite3_bind_int64(s, i, v) : sqlite3_bind_null(s, i);
    if (rc != SQLITE_OK) {
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind an identifier");
    }
    return ATLAS_OK;
}

static atlas_status bind_blob_opt(atlas_db *db, sqlite3_stmt *s, int i, const void *p, size_t n,
                                  atlas_err *err) {
    if (p == NULL) {
        if (sqlite3_bind_null(s, i) != SQLITE_OK) {
            return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind a null blob");
        }
        return ATLAS_OK;
    }
    return atlas_db_bind_blob(db, s, i, p, n, err);
}

static int64_t clamp_limit(int64_t limit, int64_t def, int64_t max) {
    if (limit <= 0) {
        return def;
    }
    return limit > max ? max : limit;
}

/* Runs a statement that yields at most one integer, taking one integer. */
static atlas_status query_int_1(atlas_db *db, const char *sql, int64_t arg, int64_t *out,
                                atlas_err *err) {
    *out = 0;
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, sql, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, stmt, 1, arg, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *out = sqlite3_column_int64(stmt, 0);
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read a count");
    }
    atlas_db_finish(db, stmt);
    return st;
}

/* The last component of a path, as raw bytes.
 *
 * Stored alongside the path so a suffix match becomes an index seek. Repeated
 * here rather than shared with `src/code/code.c`'s copy because that one is
 * about roles and this one is about a stored column, and one function serving
 * both would tie a storage decision to a classification decision. */
static void basename_of(const void *path, size_t len, const void **out, size_t *out_len) {
    const char *p = (const char *)path;
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        if (p[i] == '/') {
            start = i + 1u;
        }
    }
    *out = p + start;
    *out_len = len - start;
}

/* Runs a prepared DELETE that takes exactly one integer. */
static atlas_status exec_delete_1(atlas_db *db, const char *sql, int64_t arg, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, sql, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, stmt, 1, arg, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

/* --- index state ---------------------------------------------------------- */

void atlas_code_index_state_init(atlas_code_index_state *s) {
    memset(s, 0, sizeof(*s));
    atlas_buf_init(&s->degraded_reason);
    atlas_buf_init(&s->detail);
    atlas_buf_init(&s->last_error);
    atlas_buf_init(&s->compile_db_hash);
    atlas_buf_init(&s->analyzer_name);
}

void atlas_code_index_state_free(atlas_code_index_state *s) {
    if (s == NULL) {
        return;
    }
    atlas_buf_free(&s->degraded_reason);
    atlas_buf_free(&s->detail);
    atlas_buf_free(&s->last_error);
    atlas_buf_free(&s->compile_db_hash);
    atlas_buf_free(&s->analyzer_name);
}

atlas_status atlas_db_code_state_get(atlas_db *db, int64_t repo_id, atlas_code_index_state *out,
                                     atlas_err *err) {
    out->repo_id = repo_id;
    out->present = false;
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "SELECT generation, last_complete_generation, last_indexed_at, last_complete_at,"
        " degraded, degraded_reason, detail, last_error, files_indexed, files_parsed_last,"
        " symbols, relations, ambiguous, unresolved, compile_db_present, compile_db_hash,"
        " compile_units, compile_entries_dropped, resolve_settled,"
        " (SELECT a.name FROM code_analyzers a WHERE a.id = s.analyzer_id),"
        " (SELECT a.version FROM code_analyzers a WHERE a.id = s.analyzer_id)"
        " FROM code_index_state s WHERE s.repo_id=?1;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, stmt, 1, repo_id, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        out->present = true;
        out->generation = sqlite3_column_int64(stmt, 0);
        out->last_complete_generation = sqlite3_column_int64(stmt, 1);
        (void)snprintf(out->last_indexed_at, sizeof(out->last_indexed_at), "%s",
                       atlas_db_col_text(stmt, 2));
        (void)snprintf(out->last_complete_at, sizeof(out->last_complete_at), "%s",
                       atlas_db_col_text(stmt, 3));
        out->degraded = sqlite3_column_int(stmt, 4) != 0;
        st = atlas_buf_set_str(&out->degraded_reason, atlas_db_col_text(stmt, 5), err);
        if (st == ATLAS_OK) {
            st = atlas_buf_set_str(&out->detail, atlas_db_col_text(stmt, 6), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_set_str(&out->last_error, atlas_db_col_text(stmt, 7), err);
        }
        out->files_indexed = sqlite3_column_int64(stmt, 8);
        out->files_parsed_last = sqlite3_column_int64(stmt, 9);
        out->symbols = sqlite3_column_int64(stmt, 10);
        out->relations = sqlite3_column_int64(stmt, 11);
        out->ambiguous = sqlite3_column_int64(stmt, 12);
        out->unresolved = sqlite3_column_int64(stmt, 13);
        out->compile_db_present = sqlite3_column_int(stmt, 14) != 0;
        if (st == ATLAS_OK) {
            st = atlas_buf_set_str(&out->compile_db_hash, atlas_db_col_text(stmt, 15), err);
        }
        out->compile_units = sqlite3_column_int64(stmt, 16);
        out->compile_entries_dropped = sqlite3_column_int64(stmt, 17);
        out->resolve_settled = sqlite3_column_int(stmt, 18) != 0;
        if (st == ATLAS_OK) {
            st = atlas_buf_set_str(&out->analyzer_name, atlas_db_col_text(stmt, 19), err);
        }
        out->analyzer_version = sqlite3_column_int64(stmt, 20);
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read the structural index state");
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_code_state_ensure(atlas_db *db, int64_t repo_id, atlas_err *err) {
    return exec_delete_1(db,
                         "INSERT INTO code_index_state(repo_id) VALUES(?1)"
                         " ON CONFLICT(repo_id) DO NOTHING;",
                         repo_id, err);
}

atlas_status atlas_db_code_analyzer_intern(atlas_db *db, const char *name, int64_t version,
                                           int64_t *id_out, atlas_err *err) {
    *id_out = 0;
    sqlite3_stmt *stmt = NULL;
    /* Insert-or-ignore then select, rather than an upsert with RETURNING: the
     * row is immutable once written — an analyzer identity is a historical fact
     * about what built something, not a mutable setting — so there is nothing
     * for an update branch to do. */
    atlas_status st = atlas_db_prepare(
        db, "INSERT INTO code_analyzers(name, version, first_seen_at) VALUES(?1,?2,?3)"
            " ON CONFLICT(name, version) DO NOTHING;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));
    st = atlas_db_bind_text_opt(db, stmt, 1, name, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 2, version, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, now, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    st = atlas_db_step_done(db, stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }

    stmt = NULL;
    st = atlas_db_prepare(db, "SELECT id FROM code_analyzers WHERE name=?1 AND version=?2;", &stmt,
                          err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_bind_text_opt(db, stmt, 1, name, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 2, version, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *id_out = sqlite3_column_int64(stmt, 0);
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read the analyzer identity");
    }
    atlas_db_finish(db, stmt);
    if (st == ATLAS_OK && *id_out <= 0) {
        st = atlas_err_set(err, ATLAS_ERR_DB, "the analyzer identity was not recorded");
    }
    return st;
}

atlas_status atlas_db_code_state_set_analyzer(atlas_db *db, int64_t repo_id, int64_t analyzer_id,
                                              atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db, "UPDATE code_index_state SET analyzer_id=?2 WHERE repo_id=?1;", &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, stmt, 1, repo_id, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 2, analyzer_id, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_code_state_begin(atlas_db *db, int64_t repo_id, int64_t generation,
                                       atlas_err *err) {
    atlas_status st = atlas_db_code_state_ensure(db, repo_id, err);
    if (st != ATLAS_OK) {
        return st;
    }
    sqlite3_stmt *stmt = NULL;
    st = atlas_db_prepare(db,
                          /* `resolve_settled` is cleared here, before any work,
                           * and set again only when the pass finishes. A pass
                           * that dies half way through resolution therefore
                           * leaves it 0, and the next pass resolves rather than
                           * trusting a flag the dead pass never earned. */
                          "UPDATE code_index_state SET generation=?2, last_indexed_at=?3,"
                          " resolve_settled=0 WHERE repo_id=?1;",
                          &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));
    st = bind_i64(db, stmt, 1, repo_id, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 2, generation, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, now, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_code_state_complete(atlas_db *db, int64_t repo_id, int64_t generation,
                                          int64_t files_parsed, bool degraded,
                                          const char *degraded_reason, const char *detail,
                                          bool recount, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    /* The counters are recomputed from the tables rather than incremented as
     * rows are written. An incremented counter drifts the first time a path
     * fails part way through and nothing ever notices; five indexed COUNT(*)
     * queries are cheaper than a number nobody can trust.
     *
     * They are recomputed on every pass that could have written a row, and
     * skipped on one that provably could not: no file parsed, none removed, no
     * compile database change, and resolution already settled. Two of the five
     * counts are full scans of the relation table by design — there is no index
     * on `resolution` — so at half a million edges they are most of what an
     * otherwise empty pass costs, and running them to confirm a number that
     * cannot have moved is the same mistake as reparsing an unchanged file.
     *
     * `last_complete_generation` advances with max(), never assignment, so a
     * slow pass finishing after a newer one cannot move the published state
     * backwards. Same rule as `repo_index_state`. */
    static const char COMPLETE_RECOUNT_SQL[] =
        "UPDATE code_index_state SET"
        " generation=max(generation, ?2),"
        " last_complete_generation=max(last_complete_generation, ?2),"
        " last_complete_at=?3, last_indexed_at=?3,"
        " files_parsed_last=?4, degraded=?5, degraded_reason=?6, detail=?7,"
        " resolve_settled=1,"
        " files_indexed=(SELECT COUNT(*) FROM code_files WHERE repo_id=?1),"
        " symbols=(SELECT COUNT(*) FROM code_symbols WHERE repo_id=?1),"
        " relations=(SELECT COUNT(*) FROM code_relations WHERE repo_id=?1),"
        " ambiguous=(SELECT COUNT(*) FROM code_relations WHERE repo_id=?1"
        "            AND resolution='AMBIGUOUS'),"
        " unresolved=(SELECT COUNT(*) FROM code_relations WHERE repo_id=?1"
        "             AND resolution='UNRESOLVED')"
        " WHERE repo_id=?1;";
    static const char COMPLETE_SQL[] =
        "UPDATE code_index_state SET"
        " generation=max(generation, ?2),"
        " last_complete_generation=max(last_complete_generation, ?2),"
        " last_complete_at=?3, last_indexed_at=?3,"
        " files_parsed_last=?4, degraded=?5, degraded_reason=?6, detail=?7,"
        " resolve_settled=1"
        " WHERE repo_id=?1;";
    atlas_status st =
        atlas_db_prepare(db, recount ? COMPLETE_RECOUNT_SQL : COMPLETE_SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));
    st = bind_i64(db, stmt, 1, repo_id, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 2, generation, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, now, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 4, files_parsed, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 5, degraded ? 1 : 0, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 6, degraded ? degraded_reason : NULL, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 7, detail, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_code_state_set_error(atlas_db *db, int64_t repo_id, const char *detail,
                                           atlas_err *err) {
    atlas_status st = atlas_db_code_state_ensure(db, repo_id, err);
    if (st != ATLAS_OK) {
        return st;
    }
    sqlite3_stmt *stmt = NULL;
    st = atlas_db_prepare(db,
                          "UPDATE code_index_state SET last_error=?2, degraded=1,"
                          " degraded_reason=COALESCE(degraded_reason,'a structural pass failed')"
                          " WHERE repo_id=?1;",
                          &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, stmt, 1, repo_id, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 2, detail, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_code_state_set_compile_db(atlas_db *db, int64_t repo_id, bool present,
                                                const char *hash, int64_t units, int64_t dropped,
                                                atlas_err *err) {
    atlas_status st = atlas_db_code_state_ensure(db, repo_id, err);
    if (st != ATLAS_OK) {
        return st;
    }
    sqlite3_stmt *stmt = NULL;
    st = atlas_db_prepare(db,
                          "UPDATE code_index_state SET compile_db_present=?2, compile_db_hash=?3,"
                          " compile_units=?4, compile_entries_dropped=?5 WHERE repo_id=?1;",
                          &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, stmt, 1, repo_id, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 2, present ? 1 : 0, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, hash, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 4, units, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 5, dropped, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

/* --- selection ------------------------------------------------------------ */

/* Fills a todo row from a statement whose columns are, in order:
 * file_id, code_file_id, path_raw, path_text, content_hash. */
static void todo_from_stmt(atlas_code_todo_row *row, sqlite3_stmt *stmt) {
    memset(row, 0, sizeof(*row));
    row->file_id = sqlite3_column_int64(stmt, 0);
    row->code_file_id = sqlite3_column_int64(stmt, 1);
    row->path_raw = sqlite3_column_blob(stmt, 2);
    row->path_raw_len = (size_t)sqlite3_column_bytes(stmt, 2);
    row->path_text = atlas_db_col_text(stmt, 3);
    row->content_hash = atlas_db_col_text_opt(stmt, 4);
}

atlas_status atlas_db_code_files_to_parse(atlas_db *db, int64_t repo_id, int64_t limit,
                                          atlas_code_todo_cb cb, void *ud, int64_t *count_out,
                                          bool *more_out, atlas_err *err) {
    *count_out = 0;
    *more_out = false;
    int64_t want = clamp_limit(limit, ATLAS_CODE_MAX_PARSE_FILES_PER_PASS,
                               ATLAS_CODE_MAX_PARSE_FILES_PER_PASS);

    sqlite3_stmt *stmt = NULL;
    /* The whole incremental guarantee, in one comparison.
     *
     * A file is selected when the graph has no facts for it, or when the hash
     * the facts were extracted from differs from the hash the file index holds
     * now. Not "was it hashed this pass": a full content-verifying pass rehashes
     * every byte and finds the same hash, so an unchanged repository still
     * selects nothing. Keying off the pass's own activity instead would make a
     * periodic full pass reparse the world every five minutes.
     *
     * The LIKE clauses are a cheap pre-filter on the supported extensions; the
     * caller applies `atlas_code_language_of` to the raw bytes, which is
     * case-sensitive and is the authority. SQLite's LIKE is ASCII
     * case-insensitive, so `.C` reaches the caller and is rejected there —
     * which is correct, because `.C` is C++ by convention and A3 does not guess
     * at C++.
     *
     * One row more than the limit is requested and never delivered, so `more` is
     * a fact rather than an inference from a full page. */
    atlas_status st = atlas_db_prepare(
        db,
        "SELECT f.id, COALESCE(c.id, 0), f.path_raw, f.path_text, f.content_hash"
        " FROM files f LEFT JOIN code_files c"
        "   ON c.repo_id = f.repo_id AND c.path_raw = f.path_raw"
        " WHERE f.repo_id=?1 AND f.deleted=0 AND f.content_hash IS NOT NULL"
        "   AND (f.path_text LIKE '%.c' OR f.path_text LIKE '%.h'"
        "        OR f.path_text LIKE '%.inc' OR f.path_text LIKE '%.def')"
        "   AND (c.id IS NULL OR c.content_hash IS NULL OR c.content_hash <> f.content_hash)"
        " ORDER BY f.path_raw LIMIT ?2;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, stmt, 1, repo_id, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 2, want + 1, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (*count_out >= want) {
            *more_out = true;
            break;
        }
        atlas_code_todo_row row;
        todo_from_stmt(&row, stmt);
        st = cb(&row, ud, err);
        if (st != ATLAS_OK) {
            break;
        }
        (*count_out)++;
    }
    if (st == ATLAS_OK && rc != SQLITE_ROW && rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot select files for structural indexing");
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_code_files_to_remove(atlas_db *db, int64_t repo_id, atlas_code_todo_cb cb,
                                           void *ud, int64_t *count_out, atlas_err *err) {
    *count_out = 0;
    sqlite3_stmt *stmt = NULL;
    /* A rename is a tombstone plus an addition in `files`, so it arrives here as
     * a removal and in the parse selection as an addition. Nothing has to
     * recognise a rename as such, which is what keeps the stale-row case
     * impossible rather than handled. */
    atlas_status st = atlas_db_prepare(
        db,
        "SELECT 0, c.id, c.path_raw, c.path_text, c.content_hash"
        " FROM code_files c LEFT JOIN files f"
        "   ON f.repo_id = c.repo_id AND f.path_raw = c.path_raw AND f.deleted = 0"
        " WHERE c.repo_id=?1 AND f.id IS NULL"
        " ORDER BY c.path_raw;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, stmt, 1, repo_id, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        atlas_code_todo_row row;
        todo_from_stmt(&row, stmt);
        st = cb(&row, ud, err);
        if (st != ATLAS_OK) {
            break;
        }
        (*count_out)++;
    }
    if (st == ATLAS_OK && rc != SQLITE_ROW && rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot select stale structural rows");
    }
    atlas_db_finish(db, stmt);
    return st;
}

/* --- per-file replacement -------------------------------------------------- */

atlas_status atlas_db_code_file_upsert(atlas_db *db, int64_t repo_id,
                                       const atlas_code_file_record *rec, int64_t *id_out,
                                       atlas_err *err) {
    *id_out = 0;
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "INSERT INTO code_files(repo_id, file_id, path_raw, path_text, language, content_hash,"
        " generation, parsed_at, parse_status, parse_detail, truncated, truncated_reason,"
        " include_guard, symbol_count, include_count, occurrence_count, bytes, lines,"
        " basename_raw)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19)"
        " ON CONFLICT(repo_id, path_raw) DO UPDATE SET"
        " file_id=excluded.file_id, path_text=excluded.path_text, language=excluded.language,"
        " content_hash=excluded.content_hash, generation=excluded.generation,"
        " parsed_at=excluded.parsed_at, parse_status=excluded.parse_status,"
        " parse_detail=excluded.parse_detail, truncated=excluded.truncated,"
        " truncated_reason=excluded.truncated_reason, include_guard=excluded.include_guard,"
        " symbol_count=excluded.symbol_count, include_count=excluded.include_count,"
        " occurrence_count=excluded.occurrence_count, bytes=excluded.bytes,"
        " lines=excluded.lines"
        " RETURNING id;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));
    st = bind_i64(db, stmt, 1, repo_id, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 2, rec->file_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_blob(db, stmt, 3, rec->path_raw, rec->path_raw_len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 4, rec->path_text, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 5, rec->language, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 6, rec->content_hash, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 7, rec->generation, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 8, now, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 9, rec->parse_status, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 10, rec->parse_detail, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 11, rec->truncated ? 1 : 0, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 12, rec->truncated_reason, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 13, rec->include_guard ? 1 : 0, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 14, rec->symbol_count, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 15, rec->include_count, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 16, rec->occurrence_count, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 17, rec->bytes, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 18, rec->lines, err);
    }
    if (st == ATLAS_OK) {
        const void *base = NULL;
        size_t base_len = 0;
        basename_of(rec->path_raw, rec->path_raw_len, &base, &base_len);
        st = atlas_db_bind_blob(db, stmt, 19, base, base_len, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *id_out = sqlite3_column_int64(stmt, 0);
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot record a structurally indexed file");
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_code_file_clear(atlas_db *db, int64_t code_file_id, atlas_err *err) {
    /* Relations first: candidates cascade from them, and deleting symbols first
     * would leave a window in which an edge points at a row that is gone. Inside
     * one transaction that window is not observable, and doing it in the safe
     * order anyway costs nothing and survives being called from somewhere else. */
    atlas_status st = exec_delete_1(db, "DELETE FROM code_relations WHERE owner_file_id=?1;",
                                    code_file_id, err);
    if (st == ATLAS_OK) {
        st = exec_delete_1(db, "DELETE FROM code_occurrences WHERE code_file_id=?1;", code_file_id,
                           err);
    }
    if (st == ATLAS_OK) {
        st = exec_delete_1(db, "DELETE FROM code_symbols WHERE code_file_id=?1;", code_file_id, err);
    }
    if (st == ATLAS_OK) {
        st = exec_delete_1(db, "DELETE FROM code_file_roles WHERE code_file_id=?1;", code_file_id,
                           err);
    }
    return st;
}

atlas_status atlas_db_code_file_delete(atlas_db *db, int64_t code_file_id, atlas_err *err) {
    atlas_status st = atlas_db_code_file_clear(db, code_file_id, err);
    if (st == ATLAS_OK) {
        st = exec_delete_1(db, "DELETE FROM code_files WHERE id=?1;", code_file_id, err);
    }
    return st;
}

atlas_status atlas_db_code_clear_repo(atlas_db *db, int64_t repo_id, atlas_err *err) {
    /* Deleting the `code_files` rows cascades to symbols, occurrences,
     * relations, candidates and roles, because every one of those references
     * `code_files(id) ON DELETE CASCADE`. Units are separate: they come from the
     * compile database rather than from a parse, so they are cleared explicitly
     * and the recorded compile-database hash is reset so the next pass ingests
     * it again rather than believing it is unchanged. */
    atlas_status st = exec_delete_1(db, "DELETE FROM code_files WHERE repo_id=?1;", repo_id, err);
    if (st == ATLAS_OK) {
        st = exec_delete_1(db, "DELETE FROM code_units WHERE repo_id=?1;", repo_id, err);
    }
    if (st == ATLAS_OK) {
        st = exec_delete_1(db, "DELETE FROM code_index_errors WHERE repo_id=?1;", repo_id, err);
    }
    if (st == ATLAS_OK) {
        st = exec_delete_1(db,
                           "UPDATE code_index_state SET last_complete_generation=0, generation=0,"
                           " compile_db_hash=NULL, compile_db_present=0, compile_units=0,"
                           " degraded=0, degraded_reason=NULL, detail=NULL"
                           " WHERE repo_id=?1;",
                           repo_id, err);
    }
    return st;
}

atlas_status atlas_db_code_role_add(atlas_db *db, int64_t code_file_id, const char *role,
                                    const char *basis, const char *resolution, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "INSERT INTO code_file_roles(code_file_id, role, basis, resolution) VALUES(?1,?2,?3,?4)"
        " ON CONFLICT(code_file_id, role, basis) DO UPDATE SET resolution=excluded.resolution;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, stmt, 1, code_file_id, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 2, role, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, basis, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 4, resolution, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_code_symbol_insert(atlas_db *db, int64_t repo_id, int64_t code_file_id,
                                         const atlas_code_symbol_record *rec, int64_t *id_out,
                                         atlas_err *err) {
    *id_out = 0;
    sqlite3_stmt *stmt = NULL;
    /* ON CONFLICT DO NOTHING plus a RETURNING would give no id on a collision,
     * so the conflict updates instead. A collision means the same file declared
     * the same name of the same kind at the same byte offset twice, which can
     * only happen on a replay, and the right answer there is the row it already
     * has rather than a second one. */
    atlas_status st = atlas_db_prepare(
        db,
        "INSERT INTO code_symbols(repo_id, code_file_id, name, name_text, kind, linkage,"
        " resolution, is_definition, is_declaration, line, col, byte_offset, end_line,"
        " enclosing_id, generation)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15)"
        " ON CONFLICT(code_file_id, kind, name, byte_offset) DO UPDATE SET"
        " linkage=excluded.linkage, resolution=excluded.resolution,"
        " is_definition=excluded.is_definition, is_declaration=excluded.is_declaration,"
        " line=excluded.line, col=excluded.col, end_line=excluded.end_line,"
        " enclosing_id=excluded.enclosing_id, generation=excluded.generation"
        " RETURNING id;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, stmt, 1, repo_id, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 2, code_file_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_blob(db, stmt, 3, rec->name, rec->name_len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 4, rec->name_text, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 5, rec->kind, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 6, rec->linkage, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 7, rec->resolution, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 8, rec->is_definition ? 1 : 0, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 9, rec->is_declaration ? 1 : 0, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 10, rec->line, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 11, rec->col, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 12, rec->byte_offset, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 13, rec->end_line, err);
    }
    if (st == ATLAS_OK) {
        st = bind_id_opt(db, stmt, 14, rec->enclosing_id, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 15, rec->generation, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *id_out = sqlite3_column_int64(stmt, 0);
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot record a symbol");
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_code_occurrence_insert(atlas_db *db, int64_t repo_id, int64_t code_file_id,
                                             const atlas_code_occurrence_record *rec,
                                             int64_t *id_out, atlas_err *err) {
    *id_out = 0;
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "INSERT INTO code_occurrences(repo_id, code_file_id, enclosing_id, name, name_text,"
        " kind, resolution, line, col, byte_offset, generation)"
        " VALUES(?1,?2,?3,?4,?5,'call_candidate',?6,?7,?8,?9,?10)"
        " ON CONFLICT(code_file_id, byte_offset) DO UPDATE SET"
        " enclosing_id=excluded.enclosing_id, name=excluded.name, name_text=excluded.name_text,"
        " resolution=excluded.resolution, line=excluded.line, col=excluded.col,"
        " generation=excluded.generation"
        " RETURNING id;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, stmt, 1, repo_id, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 2, code_file_id, err);
    }
    if (st == ATLAS_OK) {
        st = bind_id_opt(db, stmt, 3, rec->enclosing_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_blob(db, stmt, 4, rec->name, rec->name_len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 5, rec->name_text, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 6, rec->resolution, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 7, rec->line, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 8, rec->col, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 9, rec->byte_offset, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 10, rec->generation, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *id_out = sqlite3_column_int64(stmt, 0);
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot record a call candidate");
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_code_relation_insert(atlas_db *db, int64_t repo_id,
                                           const atlas_code_relation_record *rec, int64_t *id_out,
                                           atlas_err *err) {
    *id_out = 0;
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "INSERT INTO code_relations(repo_id, owner_file_id, kind, src_kind, src_id, dst_kind,"
        " dst_id, dst_name, dst_name_text, resolution, provenance, candidate_count, detail,"
        " line, col, generation, spelling_form)"
        /* No RETURNING here, unlike its neighbours, and the difference is not
         * cosmetic: this is the only one of them that is a plain insert with no
         * ON CONFLICT, so `last_insert_rowid()` is the id and nothing has to
         * read it back. RETURNING makes SQLite materialise the returned row in
         * an ephemeral table, and this statement runs a hundred times per file —
         * more than every other insert in A3 put together. Its neighbours keep
         * RETURNING because an upsert may take the update branch, where the
         * last inserted rowid is not the row's. */
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17);",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, stmt, 1, repo_id, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 2, rec->owner_file_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, rec->kind, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 4, rec->src_kind, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 5, rec->src_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 6, rec->dst_kind, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 7, rec->dst_id, err);
    }
    if (st == ATLAS_OK) {
        st = bind_blob_opt(db, stmt, 8, rec->dst_name, rec->dst_name_len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 9, rec->dst_name_text, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 10, rec->resolution, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 11, rec->provenance, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 12, rec->candidate_count, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 13, rec->detail, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 14, rec->line, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 15, rec->col, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 16, rec->generation, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 17, rec->spelling_form, err);
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

atlas_status atlas_db_code_candidate_add(atlas_db *db, int64_t relation_id, const char *node_kind,
                                         int64_t node_id, int64_t rank, const char *detail,
                                         atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "INSERT INTO code_candidates(relation_id, node_kind, node_id, rank, detail)"
        " VALUES(?1,?2,?3,?4,?5)"
        " ON CONFLICT(relation_id, node_kind, node_id) DO UPDATE SET rank=excluded.rank;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, stmt, 1, relation_id, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 2, node_kind, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 3, node_id, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 4, rank, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 5, detail, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

/* --- resolution ------------------------------------------------------------ */

static void pending_from_stmt(atlas_code_pending_row *row, sqlite3_stmt *stmt) {
    memset(row, 0, sizeof(*row));
    row->id = sqlite3_column_int64(stmt, 0);
    row->owner_file_id = sqlite3_column_int64(stmt, 1);
    row->kind = atlas_db_col_text(stmt, 2);
    row->src_kind = atlas_db_col_text(stmt, 3);
    row->src_id = sqlite3_column_int64(stmt, 4);
    row->dst_name = sqlite3_column_blob(stmt, 5);
    row->dst_name_len = (size_t)sqlite3_column_bytes(stmt, 5);
    row->dst_name_text = atlas_db_col_text(stmt, 6);
    row->resolution = atlas_db_col_text(stmt, 7);
    row->line = sqlite3_column_int64(stmt, 8);
    row->col = sqlite3_column_int64(stmt, 9);
    row->owner_path_raw = sqlite3_column_blob(stmt, 10);
    row->owner_path_len = (size_t)sqlite3_column_bytes(stmt, 10);
    row->spelling_form = atlas_db_col_text_opt(stmt, 11);
    row->candidate_count = sqlite3_column_int64(stmt, 12);
}

#define PENDING_COLUMNS                                                                            \
    "r.id, r.owner_file_id, r.kind, r.src_kind, r.src_id, r.dst_name, r.dst_name_text,"            \
    " r.resolution, r.line, r.col, o.path_raw, r.spelling_form, r.candidate_count"

atlas_status atlas_db_code_relations_pending(atlas_db *db, int64_t repo_id, const char *kind,
                                             int64_t owner_file_id, atlas_code_sweep sweep,
                                             const void *name, size_t name_len, int64_t after_id,
                                             int64_t limit, atlas_code_pending_cb cb, void *ud,
                                             int64_t *count_out, int64_t *cursor_out,
                                             atlas_err *err) {
    *count_out = 0;
    if (cursor_out != NULL) {
        *cursor_out = after_id;
    }
    sqlite3_stmt *stmt = NULL;
    /* Two statements rather than one with an optional predicate.
     *
     * `(?N IS NULL OR r.dst_name = ?N)` reads as one tidy statement and is not:
     * SQLite cannot use `idx_code_rel_name` through an OR whose other branch is
     * unconstrained, so the name form degenerated into a scan of every edge of
     * that kind — once per changed symbol name. On a first pass that is
     * thousands of scans of tens of thousands of rows, and it was, measured, the
     * dominant cost of indexing a large repository.
     *
     * Splitting them lets the name form be an index seek, which is what the
     * whole by-name re-resolution strategy assumed it already was. */
    static const char PENDING_SQL[] =
        "SELECT " PENDING_COLUMNS
        " FROM code_relations r JOIN code_files o ON o.id = r.owner_file_id"
        " WHERE r.repo_id=?1 AND r.kind=?2 AND r.id > ?6"
        "   AND (?3 = 0 OR r.owner_file_id = ?3)"
        "   AND (?4 = 0"
        "        OR (?4 = 1 AND r.resolution IN ('UNRESOLVED','AMBIGUOUS'))"
        "        OR (?4 = 2 AND r.resolution NOT IN ('UNRESOLVED','AMBIGUOUS')))"
        " ORDER BY r.id LIMIT ?7;";
    static const char PENDING_BY_NAME_SQL[] =
        "SELECT " PENDING_COLUMNS
        " FROM code_relations r JOIN code_files o ON o.id = r.owner_file_id"
        " WHERE r.repo_id=?1 AND r.kind=?2 AND r.dst_name=?5 AND r.id > ?6"
        "   AND (?3 = 0 OR r.owner_file_id = ?3)"
        "   AND (?4 = 0"
        "        OR (?4 = 1 AND r.resolution IN ('UNRESOLVED','AMBIGUOUS'))"
        "        OR (?4 = 2 AND r.resolution NOT IN ('UNRESOLVED','AMBIGUOUS')))"
        " ORDER BY r.id LIMIT ?7;";
    bool by_name = (name != NULL && name_len > 0);
    atlas_status st = atlas_db_prepare(db, by_name ? PENDING_BY_NAME_SQL : PENDING_SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, stmt, 1, repo_id, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 2, kind, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 3, owner_file_id, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 4, (int64_t)sweep, err);
    }
    if (st == ATLAS_OK && by_name) {
        st = atlas_db_bind_blob(db, stmt, 5, name, name_len, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 6, after_id, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 7, limit > 0 ? limit : ATLAS_DB_BATCH_MAX, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        atlas_code_pending_row row;
        pending_from_stmt(&row, stmt);
        if (cursor_out != NULL) {
            *cursor_out = row.id;
        }
        st = cb(&row, ud, err);
        if (st != ATLAS_OK) {
            break;
        }
        (*count_out)++;
    }
    if (st == ATLAS_OK && rc != SQLITE_ROW && rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot select relations for resolution");
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_code_relations_unsettle_for_file(atlas_db *db, int64_t repo_id,
                                                       int64_t code_file_id,
                                                       bool include_file_itself,
                                                       int64_t generation, int64_t *count_out,
                                                       atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    /* One statement, and it is an UPDATE rather than a stream-and-write for a
     * reason: the rows to change are identified entirely by the destination they
     * point at, so there is nothing for a callback to decide. The subquery is a
     * seek on `idx_code_symbols_file`, and each id it yields is a seek on
     * `idx_code_rel_dst`.
     *
     * `resolution` goes back to UNRESOLVED rather than to what it was, because
     * what it was is no longer known to be true — that is the same rule
     * `settle` follows, and the resolver will decide again in this same pass. */
#define UNSETTLE_WHERE_SYMBOL                                                                      \
    " WHERE repo_id=?1 AND dst_kind='symbol'"                                                      \
    "   AND dst_id IN (SELECT id FROM code_symbols WHERE code_file_id=?2)"
#define UNSETTLE_WHERE_FILE " WHERE repo_id=?1 AND dst_kind='file' AND dst_id=?2"
#define UNSETTLE_SET                                                                               \
    "UPDATE code_relations SET dst_kind='unresolved', dst_id=0, resolution='UNRESOLVED',"          \
    " provenance='SOURCE', candidate_count=0, detail=NULL, generation=?3"
    /* The candidate rows go first, and they have to: `candidate_count` is set to
     * zero by the update below, and the resolver reads that count to decide
     * whether an edge has candidates worth clearing. Leaving the rows behind
     * while zeroing the count would strand them where nothing would ever look
     * again. */
    static const char UNSETTLE_CAND_SQL[] =
        "DELETE FROM code_candidates WHERE relation_id IN"
        " (SELECT id FROM code_relations" UNSETTLE_WHERE_SYMBOL " AND candidate_count > 0);";
    static const char UNSETTLE_CAND_FILE_SQL[] =
        "DELETE FROM code_candidates WHERE relation_id IN"
        " (SELECT id FROM code_relations" UNSETTLE_WHERE_FILE " AND candidate_count > 0);";
    static const char UNSETTLE_SQL[] = UNSETTLE_SET UNSETTLE_WHERE_SYMBOL ";";
    static const char UNSETTLE_FILE_SQL[] = UNSETTLE_SET UNSETTLE_WHERE_FILE ";";
#undef UNSETTLE_SET
#undef UNSETTLE_WHERE_SYMBOL
#undef UNSETTLE_WHERE_FILE

    for (int pass = 0; pass < (include_file_itself ? 2 : 1); pass++) {
        sqlite3_stmt *cand = NULL;
        atlas_status cst =
            atlas_db_prepare(db, pass == 0 ? UNSETTLE_CAND_SQL : UNSETTLE_CAND_FILE_SQL, &cand,
                             err);
        if (cst == ATLAS_OK) {
            cst = bind_i64(db, cand, 1, repo_id, err);
        }
        if (cst == ATLAS_OK) {
            cst = bind_i64(db, cand, 2, code_file_id, err);
        }
        if (cst != ATLAS_OK) {
            atlas_db_finish(db, cand);
            return cst;
        }
        cst = atlas_db_step_done(db, cand, err);
        if (cst != ATLAS_OK) {
            return cst;
        }

        sqlite3_stmt *stmt = NULL;
        atlas_status st =
            atlas_db_prepare(db, pass == 0 ? UNSETTLE_SQL : UNSETTLE_FILE_SQL, &stmt, err);
        if (st == ATLAS_OK) {
            st = bind_i64(db, stmt, 1, repo_id, err);
        }
        if (st == ATLAS_OK) {
            st = bind_i64(db, stmt, 2, code_file_id, err);
        }
        if (st == ATLAS_OK) {
            st = bind_i64(db, stmt, 3, generation, err);
        }
        if (st != ATLAS_OK) {
            atlas_db_finish(db, stmt);
            return st;
        }
        st = atlas_db_step_done(db, stmt, err);
        if (st != ATLAS_OK) {
            return st;
        }
        if (count_out != NULL) {
            *count_out += sqlite3_changes(db->h);
        }
    }
    return ATLAS_OK;
}

atlas_status atlas_db_code_relations_dangling(atlas_db *db, int64_t repo_id,
                                              atlas_code_pending_cb cb, void *ud,
                                              int64_t *count_out, atlas_err *err) {
    *count_out = 0;
    sqlite3_stmt *stmt = NULL;
    /* The exact invalidation set after a file was reparsed or removed, found by
     * asking which resolved destinations no longer exist rather than by guessing
     * which edges might have pointed there. A left join is the whole
     * implementation, and it cannot miss one. */
    atlas_status st = atlas_db_prepare(
        db,
        "SELECT " PENDING_COLUMNS
        " FROM code_relations r JOIN code_files o ON o.id = r.owner_file_id"
        " LEFT JOIN code_symbols s ON r.dst_kind='symbol' AND s.id = r.dst_id"
        " LEFT JOIN code_files df ON r.dst_kind='file' AND df.id = r.dst_id"
        " WHERE r.repo_id=?1"
        "   AND ((r.dst_kind='symbol' AND s.id IS NULL)"
        "        OR (r.dst_kind='file' AND df.id IS NULL))"
        " ORDER BY r.id;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, stmt, 1, repo_id, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        atlas_code_pending_row row;
        pending_from_stmt(&row, stmt);
        st = cb(&row, ud, err);
        if (st != ATLAS_OK) {
            break;
        }
        (*count_out)++;
    }
    if (st == ATLAS_OK && rc != SQLITE_ROW && rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot select dangling relations");
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_code_relation_resolve(atlas_db *db, int64_t relation_id, const char *dst_kind,
                                            int64_t dst_id, const char *resolution,
                                            const char *provenance, int64_t candidate_count,
                                            const char *detail, int64_t generation,
                                            atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "UPDATE code_relations SET dst_kind=?2, dst_id=?3, resolution=?4, provenance=?5,"
        " candidate_count=?6, detail=?7, generation=?8 WHERE id=?1;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, stmt, 1, relation_id, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 2, dst_kind, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 3, dst_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 4, resolution, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 5, provenance, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 6, candidate_count, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 7, detail, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 8, generation, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_code_candidates_clear(atlas_db *db, int64_t relation_id, atlas_err *err) {
    return exec_delete_1(db, "DELETE FROM code_candidates WHERE relation_id=?1;", relation_id, err);
}

atlas_status atlas_db_code_depends_clear(atlas_db *db, int64_t owner_file_id, atlas_err *err) {
    return exec_delete_1(db,
                         "DELETE FROM code_relations WHERE owner_file_id=?1"
                         " AND kind='file_depends_on_file';",
                         owner_file_id, err);
}

static void match_from_stmt(atlas_code_match_row *row, sqlite3_stmt *stmt) {
    memset(row, 0, sizeof(*row));
    row->id = sqlite3_column_int64(stmt, 0);
    row->code_file_id = sqlite3_column_int64(stmt, 1);
    row->path_raw = sqlite3_column_blob(stmt, 2);
    row->path_raw_len = (size_t)sqlite3_column_bytes(stmt, 2);
    row->path_text = atlas_db_col_text(stmt, 3);
    row->kind = atlas_db_col_text_opt(stmt, 4);
    row->linkage = atlas_db_col_text_opt(stmt, 5);
    row->is_definition = sqlite3_column_int(stmt, 6) != 0;
}

static atlas_status run_match_query(atlas_db *db, sqlite3_stmt *stmt, atlas_code_match_cb cb,
                                    void *ud, int64_t *count_out, atlas_err *err) {
    atlas_status st = ATLAS_OK;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        atlas_code_match_row row;
        match_from_stmt(&row, stmt);
        st = cb(&row, ud, err);
        if (st != ATLAS_OK) {
            break;
        }
        if (count_out != NULL) {
            (*count_out)++;
        }
    }
    if (st == ATLAS_OK && rc != SQLITE_ROW && rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read resolution candidates");
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_code_files_matching(atlas_db *db, int64_t repo_id, const void *suffix,
                                          size_t suffix_len, bool exact, int64_t limit,
                                          atlas_code_match_cb cb, void *ud, int64_t *count_out,
                                          atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    sqlite3_stmt *stmt = NULL;
    /* Two shapes, two statements, and the split is not cosmetic.
     *
     * The exact form is a lookup of a whole repository-relative path, and
     * `UNIQUE(repo_id, path_raw)` answers it in one seek returning one row.
     * Folding it into the suffix statement — which must seek on the basename —
     * made the common case pay the uncommon one's cost: `#include "module.h"`
     * from a project with a hundred and sixty modules fetched and sorted a
     * hundred and sixty rows to find the one already addressable by its full
     * path, once per file. Measured, that was seventeen seconds of a
     * five-thousand-file pass.
     *
     * The suffix form is what matches `#include "atlas/buf.h"` against
     * `include/atlas/buf.h`. A suffix has no index of its own, so the basename
     * equality does the seeking and the suffix is verified exactly against the
     * few rows it returns.
     *
     * Comparison is on raw bytes with substr(), not on the safe text form: a
     * path that is not valid UTF-8 still has to resolve, and the text form is a
     * display encoding. Ordered by path bytes so an ambiguous match reports the
     * same candidate order every time. */
    static const char EXACT_SQL[] =
        "SELECT id, id, path_raw, path_text, NULL, NULL, 0 FROM code_files"
        " WHERE repo_id=?1 AND path_raw = ?2;";
    /* `INDEXED BY` rather than a hint, and rather than hoping.
     *
     * `idx_code_files_basename` is `(repo_id, basename_raw)` and
     * `UNIQUE(repo_id, path_raw)` gives an implicit index with the same leading
     * column. Left to choose, SQLite took the unique one — it seeks on `repo_id`
     * and then *scans* every file in the repository applying `basename_raw` as a
     * filter. Measured on the acceptance fixture that is 5 444 rows visited per
     * unresolvable include and 6 836 such includes: thirty-seven million row
     * visits, and it was the single largest cost of a structural pass.
     *
     * The planner is not wrong to be uncertain — without `ANALYZE` it has no
     * statistics and both indexes look alike on their first column. `INDEXED BY`
     * settles it, and it is the opposite of a fragile trick: it is a hard
     * constraint, so if the index is ever dropped or renamed this statement
     * fails to prepare with a clear error instead of quietly becoming a scan
     * again. `tests/test_code_graph.c` asserts the plan.
     *
     * The suffix itself still cannot be indexed — that is what the basename
     * equality is for. It narrows to the handful of files with that basename,
     * and `substr()` then verifies the whole suffix exactly on those. */
    static const char SUFFIX_SQL[] =
        "SELECT id, id, path_raw, path_text, NULL, NULL, 0 FROM code_files"
        " INDEXED BY idx_code_files_basename"
        " WHERE repo_id=?1 AND basename_raw = ?7"
        "   AND length(path_raw) > ?3"
        "   AND substr(path_raw, length(path_raw) - ?3 + 1) = ?5"
        " ORDER BY path_raw LIMIT ?6;";
    atlas_status st = atlas_db_prepare(db, exact ? EXACT_SQL : SUFFIX_SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, stmt, 1, repo_id, err);
    if (st == ATLAS_OK && exact) {
        st = atlas_db_bind_blob(db, stmt, 2, suffix, suffix_len, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    if (exact) {
        return run_match_query(db, stmt, cb, ud, count_out, err);
    }

    /* The suffix form compares against "/" + spelling, built here so the
     * statement does not have to concatenate blobs. */
    atlas_buf slashed = ATLAS_BUF_INIT;
    st = atlas_buf_append_ch(&slashed, '/', err);
    if (st == ATLAS_OK) {
        st = atlas_buf_append(&slashed, suffix, suffix_len, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 3, (int64_t)slashed.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_blob(db, stmt, 5, slashed.data, slashed.len, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 6, clamp_limit(limit, ATLAS_CODE_MAX_CANDIDATES + 1,
                                               ATLAS_CODE_MAX_CANDIDATES + 1),
                      err);
    }
    if (st == ATLAS_OK) {
        const void *base = NULL;
        size_t base_len = 0;
        basename_of(suffix, suffix_len, &base, &base_len);
        st = atlas_db_bind_blob(db, stmt, 7, base, base_len, err);
    }
    if (st != ATLAS_OK) {
        atlas_buf_free(&slashed);
        atlas_db_finish(db, stmt);
        return st;
    }
    st = run_match_query(db, stmt, cb, ud, count_out, err);
    atlas_buf_free(&slashed);
    return st;
}

atlas_status atlas_db_code_symbols_named(atlas_db *db, int64_t repo_id, const void *name,
                                         size_t name_len, bool definitions_only,
                                         int64_t same_file_id, int64_t limit, bool ordered,
                                         atlas_code_match_cb cb, void *ud, int64_t *count_out,
                                         atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    int64_t want = clamp_limit(limit, ATLAS_CODE_MAX_CANDIDATES + 1,
                               ATLAS_CODE_MAX_CANDIDATES + 1);

    /* Two seeks rather than one scan, and this is the difference between a
     * structural index that finishes and one that does not.
     *
     * C linkage says an internal symbol is a candidate only inside its own file
     * — which is what keeps two files' `static void helper(void)` distinct. The
     * obvious way to write that is one statement with
     * `(s.linkage <> 'internal' OR s.code_file_id = ?)`, and it is quadratic: in
     * a large C project every file has a `helper`, so every lookup of that name
     * visits one index entry per file, fetches each row, joins it to
     * `code_files`, and discards nearly all of them — once per call site.
     *
     * Split, each half is a seek. The same-file half seeks
     * `(code_file_id, name)` and returns that file's few symbols; the
     * repository-wide half seeks `(repo_id, name, linkage)` and never touches a
     * row it is going to discard. The same-file half runs first so a same-file
     * match — a genuinely stronger candidate — is reported first, which is what
     * the old ORDER BY was for and what it cost a sort to achieve. */
    /* Each half exists in an ordered and an unordered form, and the pair is the
     * difference between one index seek and one index seek plus a sort.
     *
     * Neither index can satisfy the ORDER BY — the by-file index is
     * `(code_file_id, name)` and the repository-wide one is
     * `(repo_id, name, linkage, is_definition)`, while the order Atlas reports
     * candidates in is by path and byte offset. So SQLite builds a temporary
     * B-tree, and it builds one *per lookup*: a quarter of a million times on
     * the acceptance fixture, almost always to sort zero or one row.
     *
     * The caller resolves that by asking unordered first and asking again,
     * ordered, only when more than one candidate exists. Zero or one candidate
     * has no order to get wrong, and the ambiguous path — the only one where
     * candidate order is reported — runs exactly the query it always did. The
     * fast path is not a different answer; it is the same answer without a sort
     * nobody could observe. */
    static const char SAME_FILE_SQL[] =
        "SELECT s.id, s.code_file_id, f.path_raw, f.path_text, s.kind, s.linkage, s.is_definition"
        " FROM code_symbols s JOIN code_files f ON f.id = s.code_file_id"
        " WHERE s.code_file_id=?4 AND s.name=?2"
        "   AND (?3 = 0 OR s.is_definition = 1)"
        " ORDER BY s.byte_offset, s.id LIMIT ?5;";
    static const char SAME_FILE_UNORDERED_SQL[] =
        "SELECT s.id, s.code_file_id, f.path_raw, f.path_text, s.kind, s.linkage, s.is_definition"
        " FROM code_symbols s JOIN code_files f ON f.id = s.code_file_id"
        " WHERE s.code_file_id=?4 AND s.name=?2"
        "   AND (?3 = 0 OR s.is_definition = 1)"
        " LIMIT ?5;";
    static const char REPO_WIDE_SQL[] =
        "SELECT s.id, s.code_file_id, f.path_raw, f.path_text, s.kind, s.linkage, s.is_definition"
        " FROM code_symbols s JOIN code_files f ON f.id = s.code_file_id"
        " WHERE s.repo_id=?1 AND s.name=?2 AND s.linkage <> 'internal'"
        "   AND (?3 = 0 OR s.is_definition = 1)"
        "   AND s.code_file_id <> ?4"
        " ORDER BY f.path_raw, s.byte_offset, s.id LIMIT ?5;";
    static const char REPO_WIDE_UNORDERED_SQL[] =
        "SELECT s.id, s.code_file_id, f.path_raw, f.path_text, s.kind, s.linkage, s.is_definition"
        " FROM code_symbols s JOIN code_files f ON f.id = s.code_file_id"
        " WHERE s.repo_id=?1 AND s.name=?2 AND s.linkage <> 'internal'"
        "   AND (?3 = 0 OR s.is_definition = 1)"
        "   AND s.code_file_id <> ?4"
        " LIMIT ?5;";

    atlas_status st = ATLAS_OK;
    sqlite3_stmt *stmt = NULL;
    if (same_file_id > 0) {
        st = atlas_db_prepare(db, ordered ? SAME_FILE_SQL : SAME_FILE_UNORDERED_SQL, &stmt, err);
        if (st == ATLAS_OK) {
            st = atlas_db_bind_blob(db, stmt, 2, name, name_len, err);
        }
        if (st == ATLAS_OK) {
            st = bind_i64(db, stmt, 3, definitions_only ? 1 : 0, err);
        }
        if (st == ATLAS_OK) {
            st = bind_i64(db, stmt, 4, same_file_id, err);
        }
        if (st == ATLAS_OK) {
            st = bind_i64(db, stmt, 5, want, err);
        }
        if (st != ATLAS_OK) {
            atlas_db_finish(db, stmt);
            return st;
        }
        st = run_match_query(db, stmt, cb, ud, count_out, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }

    stmt = NULL;
    st = atlas_db_prepare(db, ordered ? REPO_WIDE_SQL : REPO_WIDE_UNORDERED_SQL, &stmt, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 1, repo_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_blob(db, stmt, 2, name, name_len, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 3, definitions_only ? 1 : 0, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 4, same_file_id, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 5, want, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return run_match_query(db, stmt, cb, ud, count_out, err);
}

/* --- compile units --------------------------------------------------------- */

atlas_status atlas_db_code_units_clear(atlas_db *db, int64_t repo_id, atlas_err *err) {
    return exec_delete_1(db, "DELETE FROM code_units WHERE repo_id=?1;", repo_id, err);
}

atlas_status atlas_db_code_link_units(atlas_db *db, int64_t repo_id, int64_t code_file_id,
                                      int64_t generation, int64_t *count_out, atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    /* Both forms delete before inserting, so the rebuild is idempotent and no
     * edge from a previous shape of the compile database survives it. The
     * per-file form deletes by `owner_file_id`, which is the unit's source file
     * — the same key the whole-repository form would reach through `kind`, but
     * as one indexed seek instead of a scan of every relation. */
    atlas_status st;
    if (code_file_id > 0) {
        st = exec_delete_1(db,
                           "DELETE FROM code_relations WHERE owner_file_id=?1"
                           " AND kind IN ('unit_compiles_file','unit_uses_header');",
                           code_file_id, err);
    } else {
        st = exec_delete_1(db,
                           "DELETE FROM code_relations WHERE repo_id=?1"
                           " AND kind IN ('unit_compiles_file','unit_uses_header');",
                           repo_id, err);
    }
    if (st != ATLAS_OK) {
        return st;
    }

    /* A unit compiles exactly the file its record names. That is what the
     * compile database says, so the edge is BUILD_METADATA on both counts. */
    sqlite3_stmt *stmt = NULL;
    st = atlas_db_prepare(
        db,
        "INSERT INTO code_relations(repo_id, owner_file_id, kind, src_kind, src_id, dst_kind,"
        " dst_id, dst_name_text, resolution, provenance, generation)"
        " SELECT u.repo_id, f.id, 'unit_compiles_file', 'unit', u.id, 'file', f.id,"
        "        u.output_text, 'BUILD_METADATA', 'BUILD_METADATA', ?2"
        " FROM code_units u JOIN code_files f"
        "   ON f.repo_id = u.repo_id AND f.path_raw = u.source_path_raw"
        " WHERE u.repo_id = ?1 AND (?3 = 0 OR f.id = ?3);",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, stmt, 1, repo_id, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 2, generation, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 3, code_file_id, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    st = atlas_db_step_done(db, stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }

    /* A unit uses the headers the file it compiles includes *directly*.
     *
     * Depth one on purpose. Deeper inclusion is a real relationship and it is
     * already visible by traversing `file_depends_on_file`, which is bounded and
     * cycle-checked; materialising the transitive closure here would be
     * quadratic in a codebase with a common header and would say nothing the
     * traversal does not. The edge is INFERENCE and its detail says so. */
    stmt = NULL;
    st = atlas_db_prepare(
        db,
        "INSERT INTO code_relations(repo_id, owner_file_id, kind, src_kind, src_id, dst_kind,"
        " dst_id, dst_name_text, resolution, provenance, detail, generation)"
        " SELECT u.repo_id, f.id, 'unit_uses_header', 'unit', u.id, 'file', r.dst_id,"
        "        r.dst_name_text, r.resolution, 'INFERENCE', 'derived_from_a_resolved_include', ?2"
        " FROM code_units u"
        " JOIN code_files f ON f.repo_id = u.repo_id AND f.path_raw = u.source_path_raw"
        " JOIN code_relations r ON r.owner_file_id = f.id"
        "   AND r.kind = 'file_includes_file' AND r.dst_kind = 'file'"
        " WHERE u.repo_id = ?1 AND (?3 = 0 OR f.id = ?3);",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, stmt, 1, repo_id, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 2, generation, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 3, code_file_id, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    st = atlas_db_step_done(db, stmt, err);
    if (st == ATLAS_OK && count_out != NULL) {
        /* What this rebuild wrote, not what the repository holds: the per-file
         * form must not report the whole graph's unit edges as its own work. */
        if (code_file_id > 0) {
            st = query_int_1(db,
                             "SELECT COUNT(*) FROM code_relations WHERE owner_file_id=?1"
                             " AND kind IN ('unit_compiles_file','unit_uses_header');",
                             code_file_id, count_out, err);
        } else {
            st = query_int_1(db,
                             "SELECT COUNT(*) FROM code_relations WHERE repo_id=?1"
                             " AND kind IN ('unit_compiles_file','unit_uses_header');",
                             repo_id, count_out, err);
        }
    }
    return st;
}

atlas_status atlas_db_code_unit_insert(atlas_db *db, int64_t repo_id,
                                       const atlas_code_unit_record *rec, int64_t *id_out,
                                       atlas_err *err) {
    *id_out = 0;
    sqlite3_stmt *stmt = NULL;
    /* A duplicate entry for one (source, output) pair collapses to one unit; a
     * second entry with a different output is a second configuration and gets
     * its own row. Collapsing those would lose exactly the distinction a compile
     * database exists to record. */
    atlas_status st = atlas_db_prepare(
        db,
        "INSERT INTO code_units(repo_id, source_path_raw, source_path_text, output_text,"
        " directory_text, language_standard, explicit_language, arg_count, dropped_args,"
        " command_present, command_hash, entry_index, generation)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13)"
        " ON CONFLICT(repo_id, source_path_raw, output_text) DO UPDATE SET"
        " directory_text=excluded.directory_text,"
        " language_standard=excluded.language_standard,"
        " explicit_language=excluded.explicit_language, arg_count=excluded.arg_count,"
        " dropped_args=excluded.dropped_args, command_present=excluded.command_present,"
        " command_hash=excluded.command_hash, generation=excluded.generation"
        " RETURNING id;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, stmt, 1, repo_id, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_blob(db, stmt, 2, rec->source_path_raw, rec->source_path_len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, rec->source_path_text, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 4, rec->output_text != NULL ? rec->output_text : "",
                                    err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 5,
                                    rec->directory_text != NULL ? rec->directory_text : "", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 6, rec->language_standard, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 7, rec->explicit_language, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 8, rec->arg_count, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 9, rec->dropped_args, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 10, rec->command_present ? 1 : 0, err);
    }
    if (st == ATLAS_OK) {
        /* The hash of the command string, never the string. Enough to notice
         * that a build line changed; incapable of being run. */
        st = atlas_db_bind_text_opt(db, stmt, 11, rec->command_hash, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 12, rec->entry_index, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 13, rec->generation, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *id_out = sqlite3_column_int64(stmt, 0);
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot record a translation unit");
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_code_unit_include_add(atlas_db *db, int64_t unit_id, const char *kind,
                                            const void *dir_raw, size_t dir_len,
                                            const char *dir_text, bool external, int64_t rank,
                                            atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "INSERT INTO code_unit_includes(unit_id, kind, dir_raw, dir_text, external, rank)"
        " VALUES(?1,?2,?3,?4,?5,?6)"
        " ON CONFLICT(unit_id, kind, dir_raw) DO UPDATE SET rank=excluded.rank,"
        " external=excluded.external;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, stmt, 1, unit_id, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 2, kind, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_blob(db, stmt, 3, dir_raw, dir_len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 4, dir_text, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 5, external ? 1 : 0, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 6, rank, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_code_unit_define_add(atlas_db *db, int64_t unit_id, const char *name,
                                           const char *value, bool undef, int64_t rank,
                                           atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "INSERT INTO code_unit_defines(unit_id, name, value, undef, rank)"
        " VALUES(?1,?2,?3,?4,?5)"
        " ON CONFLICT(unit_id, name, undef) DO UPDATE SET value=excluded.value,"
        " rank=excluded.rank;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, stmt, 1, unit_id, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 2, name, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, value, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 4, undef ? 1 : 0, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 5, rank, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_code_unit_dirs_for_file(atlas_db *db, int64_t repo_id, const void *path_raw,
                                              size_t path_len, int64_t limit,
                                              atlas_code_match_cb cb, void *ud, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    /* `external = 0` is not a filter for tidiness. An external include directory
     * is a statement about where a build looks, and Atlas is not permitted to
     * look there: no code path opens a file outside the registered repository,
     * so returning one here would be offering a resolution that must never
     * happen.
     *
     * Two statements again, and for the same reason as
     * `atlas_db_code_relations_pending`: `(?N IS NULL OR u.source_path_raw = ?N)`
     * cannot use the uniqueness index on `source_path_raw`, so the per-unit form
     * degenerated into a scan-and-sort of every unit's directories — once per
     * include edge that reached this step. That was the dominant cost of the
     * initial pass on a five-thousand-file repository, and it is invisible until
     * the repository is large enough for the scan to matter. */
    /* The per-unit form drives from `code_units` through an `IN` subquery rather
     * than from a join, because the join let SQLite choose to scan every
     * directory of every unit and filter afterwards — which is fine at ten units
     * and is seventy million row visits at five thousand. The subquery is one
     * seek on `UNIQUE(repo_id, source_path_raw, output_text)` returning a couple
     * of ids, and `idx_code_unit_inc_unit` then seeks straight to their
     * directories. */
    static const char DIRS_FOR_UNIT_SQL[] =
        "SELECT DISTINCT 0, 0, i.dir_raw, i.dir_text, i.kind, NULL, 0"
        " FROM code_unit_includes i"
        " WHERE i.external = 0"
        "   AND i.unit_id IN (SELECT u.id FROM code_units u"
        "                      WHERE u.repo_id = ?1 AND u.source_path_raw = ?2)"
        " ORDER BY i.dir_raw LIMIT ?3;";
    static const char DIRS_ALL_SQL[] =
        "SELECT DISTINCT 0, 0, i.dir_raw, i.dir_text, i.kind, NULL, 0"
        " FROM code_unit_includes i JOIN code_units u ON u.id = i.unit_id"
        " WHERE u.repo_id=?1 AND i.external = 0"
        " ORDER BY i.dir_raw LIMIT ?3;";
    bool per_unit = (path_raw != NULL && path_len > 0);
    atlas_status st = atlas_db_prepare(db, per_unit ? DIRS_FOR_UNIT_SQL : DIRS_ALL_SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, stmt, 1, repo_id, err);
    if (st == ATLAS_OK && per_unit) {
        st = atlas_db_bind_blob(db, stmt, 2, path_raw, path_len, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 3,
                      clamp_limit(limit, ATLAS_CODE_MAX_INCLUDE_DIRS_PER_UNIT,
                                  ATLAS_CODE_MAX_INCLUDE_DIRS_PER_UNIT),
                      err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return run_match_query(db, stmt, cb, ud, NULL, err);
}

/* --- errors ---------------------------------------------------------------- */

atlas_status atlas_db_code_error_add(atlas_db *db, int64_t repo_id, const char *path_text,
                                     const char *kind, const char *detail, int64_t generation,
                                     atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "INSERT INTO code_index_errors(repo_id, path_text, kind, detail, generation, created_at)"
        " VALUES(?1,?2,?3,?4,?5,?6);",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));
    st = bind_i64(db, stmt, 1, repo_id, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 2, path_text, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, kind, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 4, detail, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 5, generation, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 6, now, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_code_errors_prune(atlas_db *db, int64_t repo_id, int64_t retain,
                                        atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "DELETE FROM code_index_errors WHERE repo_id=?1 AND id NOT IN"
        " (SELECT id FROM code_index_errors WHERE repo_id=?1 ORDER BY id DESC LIMIT ?2);",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, stmt, 1, repo_id, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 2, retain > 0 ? retain : ATLAS_CODE_ERRORS_RETAIN_PER_REPO, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

/* --- queries ---------------------------------------------------------------- */

static void symbol_from_stmt(atlas_code_symbol_row *row, sqlite3_stmt *stmt) {
    memset(row, 0, sizeof(*row));
    row->id = sqlite3_column_int64(stmt, 0);
    row->code_file_id = sqlite3_column_int64(stmt, 1);
    row->path_text = atlas_db_col_text(stmt, 2);
    row->name_text = atlas_db_col_text(stmt, 3);
    row->kind = atlas_db_col_text(stmt, 4);
    row->linkage = atlas_db_col_text(stmt, 5);
    row->resolution = atlas_db_col_text(stmt, 6);
    row->is_definition = sqlite3_column_int(stmt, 7) != 0;
    row->is_declaration = sqlite3_column_int(stmt, 8) != 0;
    row->line = sqlite3_column_int64(stmt, 9);
    row->col = sqlite3_column_int64(stmt, 10);
}

#define SYMBOL_COLUMNS                                                                             \
    "s.id, s.code_file_id, f.path_text, s.name_text, s.kind, s.linkage, s.resolution,"             \
    " s.is_definition, s.is_declaration, s.line, s.col"

static atlas_status run_symbol_query(atlas_db *db, sqlite3_stmt *stmt, int64_t want,
                                     atlas_code_symbol_cb cb, void *ud, int64_t *count_out,
                                     bool *more_out, atlas_err *err) {
    *count_out = 0;
    if (more_out != NULL) {
        *more_out = false;
    }
    atlas_status st = ATLAS_OK;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (*count_out >= want) {
            if (more_out != NULL) {
                *more_out = true;
            }
            break;
        }
        atlas_code_symbol_row row;
        symbol_from_stmt(&row, stmt);
        st = cb(&row, ud, err);
        if (st != ATLAS_OK) {
            break;
        }
        (*count_out)++;
    }
    if (st == ATLAS_OK && rc != SQLITE_ROW && rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read symbols");
    }
    atlas_db_finish(db, stmt);
    return st;
}

/* Escapes LIKE metacharacters so a query is a substring search rather than a
 * pattern language a caller can be surprised by. */
static atlas_status like_pattern(const char *query, atlas_buf *out, atlas_err *err) {
    atlas_status st = atlas_buf_append_ch(out, '%', err);
    for (const char *p = query; st == ATLAS_OK && *p != '\0'; p++) {
        if (*p == '%' || *p == '_' || *p == '\\') {
            st = atlas_buf_append_ch(out, '\\', err);
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_append_ch(out, *p, err);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_append_ch(out, '%', err);
    }
    return st;
}

atlas_status atlas_db_code_symbol_search(atlas_db *db, int64_t repo_id, const char *query,
                                         const char *kind, int64_t limit,
                                         atlas_code_symbol_cb cb, void *ud, int64_t *count_out,
                                         bool *more_out, atlas_err *err) {
    *count_out = 0;
    if (more_out != NULL) {
        *more_out = false;
    }
    atlas_buf pattern = ATLAS_BUF_INIT;
    atlas_status st = like_pattern(query != NULL ? query : "", &pattern, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&pattern);
        return st;
    }
    sqlite3_stmt *stmt = NULL;
    /* Definitions before declarations, then by name, then by path: a caller
     * searching for a symbol wants the place it is defined first, and the
     * ordering must not depend on insertion order or the answer changes when
     * an unrelated file is reindexed. */
    st = atlas_db_prepare(db,
                          "SELECT " SYMBOL_COLUMNS
                          " FROM code_symbols s JOIN code_files f ON f.id = s.code_file_id"
                          " WHERE s.repo_id=?1 AND s.name_text LIKE ?2 ESCAPE '\\'"
                          "   AND (?3 IS NULL OR s.kind = ?3)"
                          " ORDER BY s.is_definition DESC, s.name_text, f.path_text, s.line"
                          " LIMIT ?4;",
                          &stmt, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&pattern);
        return st;
    }
    int64_t want = clamp_limit(limit, ATLAS_CODE_DEFAULT_ROWS, ATLAS_CODE_MAX_ROWS);
    st = bind_i64(db, stmt, 1, repo_id, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 2, atlas_buf_cstr(&pattern), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, kind, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 4, want + 1, err);
    }
    atlas_buf_free(&pattern);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return run_symbol_query(db, stmt, want, cb, ud, count_out, more_out, err);
}

atlas_status atlas_db_code_symbols_by_name(atlas_db *db, int64_t repo_id, const char *name_text,
                                           int64_t limit, atlas_code_symbol_cb cb, void *ud,
                                           int64_t *count_out, bool *more_out, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db,
                                       "SELECT " SYMBOL_COLUMNS
                                       " FROM code_symbols s JOIN code_files f"
                                       "   ON f.id = s.code_file_id"
                                       " WHERE s.repo_id=?1 AND s.name_text=?2"
                                       " ORDER BY s.is_definition DESC, f.path_text, s.line"
                                       " LIMIT ?3;",
                                       &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    int64_t want = clamp_limit(limit, ATLAS_CODE_DEFAULT_ROWS, ATLAS_CODE_MAX_ROWS);
    st = bind_i64(db, stmt, 1, repo_id, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 2, name_text, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 3, want + 1, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return run_symbol_query(db, stmt, want, cb, ud, count_out, more_out, err);
}

atlas_status atlas_db_code_symbols_in_file(atlas_db *db, int64_t code_file_id, int64_t limit,
                                           atlas_code_symbol_cb cb, void *ud, int64_t *count_out,
                                           bool *more_out, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db,
                                       "SELECT " SYMBOL_COLUMNS
                                       " FROM code_symbols s JOIN code_files f"
                                       "   ON f.id = s.code_file_id"
                                       " WHERE s.code_file_id=?1"
                                       " ORDER BY s.line, s.byte_offset LIMIT ?2;",
                                       &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    int64_t want = clamp_limit(limit, ATLAS_CODE_DEFAULT_ROWS, ATLAS_CODE_MAX_ROWS);
    st = bind_i64(db, stmt, 1, code_file_id, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 2, want + 1, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return run_symbol_query(db, stmt, want, cb, ud, count_out, more_out, err);
}

static void edge_from_stmt(atlas_code_edge_row *row, sqlite3_stmt *stmt) {
    memset(row, 0, sizeof(*row));
    row->id = sqlite3_column_int64(stmt, 0);
    row->kind = atlas_db_col_text(stmt, 1);
    row->src_kind = atlas_db_col_text(stmt, 2);
    row->src_id = sqlite3_column_int64(stmt, 3);
    row->dst_kind = atlas_db_col_text(stmt, 4);
    row->dst_id = sqlite3_column_int64(stmt, 5);
    row->dst_name_text = atlas_db_col_text_opt(stmt, 6);
    row->resolution = atlas_db_col_text(stmt, 7);
    row->provenance = atlas_db_col_text(stmt, 8);
    row->candidate_count = sqlite3_column_int64(stmt, 9);
    row->detail = atlas_db_col_text_opt(stmt, 10);
    row->line = sqlite3_column_int64(stmt, 11);
    row->col = sqlite3_column_int64(stmt, 12);
    row->src_path_text = atlas_db_col_text_opt(stmt, 13);
    row->dst_path_text = atlas_db_col_text_opt(stmt, 14);
}

#define EDGE_COLUMNS                                                                               \
    "r.id, r.kind, r.src_kind, r.src_id, r.dst_kind, r.dst_id, r.dst_name_text, r.resolution,"     \
    " r.provenance, r.candidate_count, r.detail, r.line, r.col, sf.path_text, df.path_text"
/* The two joins are LEFT joins on purpose: an edge whose source is a symbol has
 * no source file row to join, and an unresolved edge has no destination at all.
 * An inner join would silently drop exactly the rows a caller most needs. */
#define EDGE_JOINS                                                                                 \
    " FROM code_relations r"                                                                       \
    " LEFT JOIN code_files sf ON r.src_kind='file' AND sf.id = r.src_id"                           \
    " LEFT JOIN code_files df ON r.dst_kind='file' AND df.id = r.dst_id"

static atlas_status run_edge_query(atlas_db *db, sqlite3_stmt *stmt, int64_t want,
                                   atlas_code_edge_cb cb, void *ud, int64_t *count_out,
                                   bool *more_out, atlas_err *err) {
    *count_out = 0;
    if (more_out != NULL) {
        *more_out = false;
    }
    atlas_status st = ATLAS_OK;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (want > 0 && *count_out >= want) {
            if (more_out != NULL) {
                *more_out = true;
            }
            break;
        }
        atlas_code_edge_row row;
        edge_from_stmt(&row, stmt);
        st = cb(&row, ud, err);
        if (st != ATLAS_OK) {
            break;
        }
        (*count_out)++;
    }
    if (st == ATLAS_OK && rc != SQLITE_ROW && rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read structural relations");
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_code_edges_from(atlas_db *db, int64_t repo_id, const char *src_kind,
                                      int64_t src_id, const char *kind, int64_t limit,
                                      atlas_code_edge_cb cb, void *ud, int64_t *count_out,
                                      bool *more_out, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    /* Ordered by resolution first so the strongest facts come out of a truncated
     * page rather than whichever rows happened to be inserted first, then by
     * destination path bytes so the order does not depend on insertion at all.
     *
     * Two statements, for the third and last time in this file, and this one was
     * the most expensive of the three. `(?N IS NULL OR r.kind=?N)` costs the
     * planner the fourth column of `idx_code_rel_src`, and with an unindexable
     * ORDER BY on top of it SQLite abandoned the index entirely and scanned
     * every relation in the repository — once per file whose dependency edges
     * were being rebuilt. That is quadratic in the file count, and on the
     * acceptance fixture it was the single largest cost in the pass. */
    static const char FROM_ANY_SQL[] =
        "SELECT " EDGE_COLUMNS EDGE_JOINS
        " WHERE r.repo_id=?1 AND r.src_kind=?2 AND r.src_id=?3"
        " ORDER BY r.resolution, df.path_text, r.dst_name_text, r.line, r.id LIMIT ?5;";
    static const char FROM_KIND_SQL[] =
        "SELECT " EDGE_COLUMNS EDGE_JOINS
        " WHERE r.repo_id=?1 AND r.src_kind=?2 AND r.src_id=?3 AND r.kind=?4"
        " ORDER BY r.resolution, df.path_text, r.dst_name_text, r.line, r.id LIMIT ?5;";
    atlas_status st = atlas_db_prepare(db, kind != NULL ? FROM_KIND_SQL : FROM_ANY_SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    int64_t want = clamp_limit(limit, ATLAS_CODE_DEFAULT_ROWS, ATLAS_CODE_MAX_ROWS);
    st = bind_i64(db, stmt, 1, repo_id, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 2, src_kind, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 3, src_id, err);
    }
    if (st == ATLAS_OK && kind != NULL) {
        st = atlas_db_bind_text_opt(db, stmt, 4, kind, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 5, want + 1, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return run_edge_query(db, stmt, want, cb, ud, count_out, more_out, err);
}

atlas_status atlas_db_code_edges_to(atlas_db *db, int64_t repo_id, const char *dst_kind,
                                    int64_t dst_id, const char *kind, int64_t limit,
                                    atlas_code_edge_cb cb, void *ud, int64_t *count_out,
                                    bool *more_out, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    /* Split for the same reason as the outbound form. */
    static const char TO_ANY_SQL[] =
        "SELECT " EDGE_COLUMNS EDGE_JOINS
        " WHERE r.repo_id=?1 AND r.dst_kind=?2 AND r.dst_id=?3"
        " ORDER BY r.resolution, sf.path_text, r.line, r.id LIMIT ?5;";
    static const char TO_KIND_SQL[] =
        "SELECT " EDGE_COLUMNS EDGE_JOINS
        " WHERE r.repo_id=?1 AND r.dst_kind=?2 AND r.dst_id=?3 AND r.kind=?4"
        " ORDER BY r.resolution, sf.path_text, r.line, r.id LIMIT ?5;";
    atlas_status st = atlas_db_prepare(db, kind != NULL ? TO_KIND_SQL : TO_ANY_SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    int64_t want = clamp_limit(limit, ATLAS_CODE_DEFAULT_ROWS, ATLAS_CODE_MAX_ROWS);
    st = bind_i64(db, stmt, 1, repo_id, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 2, dst_kind, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 3, dst_id, err);
    }
    if (st == ATLAS_OK && kind != NULL) {
        st = atlas_db_bind_text_opt(db, stmt, 4, kind, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 5, want + 1, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return run_edge_query(db, stmt, want, cb, ud, count_out, more_out, err);
}

atlas_status atlas_db_code_candidates_of(atlas_db *db, int64_t relation_id, int64_t limit,
                                         atlas_code_edge_cb cb, void *ud, int64_t *count_out,
                                         atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    /* Shaped as an edge row so a renderer has one item type for "here is a
     * relation" and "here is one of the things it could have meant". The
     * candidate's own identity is in dst_kind/dst_id; the rest describes the
     * relation it belongs to. */
    atlas_status st = atlas_db_prepare(
        db,
        "SELECT c.id, r.kind, r.src_kind, r.src_id, c.node_kind, c.node_id, r.dst_name_text,"
        " r.resolution, r.provenance, r.candidate_count, c.detail, r.line, r.col,"
        " sf.path_text, COALESCE(cf.path_text, csf.path_text)"
        " FROM code_candidates c JOIN code_relations r ON r.id = c.relation_id"
        " LEFT JOIN code_files sf ON r.src_kind='file' AND sf.id = r.src_id"
        " LEFT JOIN code_files cf ON c.node_kind='file' AND cf.id = c.node_id"
        " LEFT JOIN code_symbols cs ON c.node_kind='symbol' AND cs.id = c.node_id"
        " LEFT JOIN code_files csf ON csf.id = cs.code_file_id"
        " WHERE c.relation_id=?1 ORDER BY c.rank, c.id LIMIT ?2;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, stmt, 1, relation_id, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 2, clamp_limit(limit, ATLAS_CODE_MAX_CANDIDATES,
                                               ATLAS_CODE_MAX_CANDIDATES),
                      err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return run_edge_query(db, stmt, 0, cb, ud, count_out, NULL, err);
}

atlas_status atlas_db_code_file_get(atlas_db *db, int64_t repo_id, const void *path_raw,
                                    size_t path_len, atlas_code_file_cb cb, void *ud, bool *found,
                                    int64_t *id_out, atlas_err *err) {
    *found = false;
    if (id_out != NULL) {
        *id_out = 0;
    }
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "SELECT id, file_id, path_raw, path_text, language, content_hash, parse_status,"
        " parse_detail, truncated, truncated_reason, include_guard, symbol_count, include_count,"
        " occurrence_count, bytes, lines, generation"
        " FROM code_files WHERE repo_id=?1 AND path_raw=?2;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, stmt, 1, repo_id, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_blob(db, stmt, 2, path_raw, path_len, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        atlas_code_file_row row;
        memset(&row, 0, sizeof(row));
        row.id = sqlite3_column_int64(stmt, 0);
        row.file_id = sqlite3_column_int64(stmt, 1);
        row.path_raw = sqlite3_column_blob(stmt, 2);
        row.path_raw_len = (size_t)sqlite3_column_bytes(stmt, 2);
        row.path_text = atlas_db_col_text(stmt, 3);
        row.language = atlas_db_col_text(stmt, 4);
        row.content_hash = atlas_db_col_text_opt(stmt, 5);
        row.parse_status = atlas_db_col_text(stmt, 6);
        row.parse_detail = atlas_db_col_text_opt(stmt, 7);
        row.truncated = sqlite3_column_int(stmt, 8) != 0;
        row.truncated_reason = atlas_db_col_text_opt(stmt, 9);
        row.include_guard = sqlite3_column_int(stmt, 10) != 0;
        row.symbol_count = sqlite3_column_int64(stmt, 11);
        row.include_count = sqlite3_column_int64(stmt, 12);
        row.occurrence_count = sqlite3_column_int64(stmt, 13);
        row.bytes = sqlite3_column_int64(stmt, 14);
        row.lines = sqlite3_column_int64(stmt, 15);
        row.generation = sqlite3_column_int64(stmt, 16);
        *found = true;
        if (id_out != NULL) {
            *id_out = row.id;
        }
        if (cb != NULL) {
            st = cb(&row, ud, err);
        }
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read a structurally indexed file");
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_code_roles_of(atlas_db *db, int64_t code_file_id, atlas_code_role_cb cb,
                                    void *ud, int64_t *count_out, atlas_err *err) {
    *count_out = 0;
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db,
                                       "SELECT role, basis, resolution FROM code_file_roles"
                                       " WHERE code_file_id=?1 ORDER BY role, basis;",
                                       &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, stmt, 1, code_file_id, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        atlas_code_role_row row;
        row.role = atlas_db_col_text(stmt, 0);
        row.basis = atlas_db_col_text(stmt, 1);
        row.resolution = atlas_db_col_text(stmt, 2);
        st = cb(&row, ud, err);
        if (st != ATLAS_OK) {
            break;
        }
        (*count_out)++;
    }
    if (st == ATLAS_OK && rc != SQLITE_ROW && rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read file roles");
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_code_units_for_file(atlas_db *db, int64_t repo_id, int64_t code_file_id,
                                          int64_t limit, atlas_code_edge_cb cb, void *ud,
                                          int64_t *count_out, bool *more_out, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "SELECT u.id, 'unit_compiles_file', 'unit', u.id, 'file', ?2, u.source_path_text,"
        " 'SOURCE_EXACT', 'BUILD_METADATA', 0, u.output_text, 0, 0, NULL, f.path_text"
        " FROM code_units u JOIN code_files f ON f.id = ?2"
        " WHERE u.repo_id=?1 AND u.source_path_raw = f.path_raw"
        " UNION ALL"
        " SELECT r.id, r.kind, r.src_kind, r.src_id, r.dst_kind, r.dst_id, r.dst_name_text,"
        " r.resolution, r.provenance, r.candidate_count, r.detail, r.line, r.col, NULL, NULL"
        " FROM code_relations r"
        " WHERE r.repo_id=?1 AND r.kind='unit_uses_header' AND r.dst_id=?2"
        " ORDER BY 7 LIMIT ?3;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    int64_t want = clamp_limit(limit, ATLAS_CODE_DEFAULT_ROWS, ATLAS_CODE_MAX_ROWS);
    st = bind_i64(db, stmt, 1, repo_id, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 2, code_file_id, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, stmt, 3, want + 1, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return run_edge_query(db, stmt, want, cb, ud, count_out, more_out, err);
}

atlas_status atlas_db_code_file_unsettled(atlas_db *db, int64_t code_file_id, int64_t *ambiguous,
                                          int64_t *unresolved, atlas_err *err) {
    *ambiguous = 0;
    *unresolved = 0;
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "SELECT SUM(resolution='AMBIGUOUS'), SUM(resolution='UNRESOLVED')"
        " FROM code_relations WHERE owner_file_id=?1;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, stmt, 1, code_file_id, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *ambiguous = sqlite3_column_int64(stmt, 0);
        *unresolved = sqlite3_column_int64(stmt, 1);
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot count unsettled relations");
    }
    atlas_db_finish(db, stmt);
    return st;
}

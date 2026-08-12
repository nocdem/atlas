/* Atlas - database internals shared between the src/db translation units.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Not a public header: sqlite3 types must not escape src/db.
 */
#ifndef ATLAS_DB_INTERNAL_H
#define ATLAS_DB_INTERNAL_H

#include <sqlite3.h>
#include <stdbool.h>

#include "atlas/db.h"

/* Distinct SQL statements one handle keeps prepared.
 *
 * Comfortably above the number of statements any one pass issues, so the hot
 * path never evicts. Past it a caller simply prepares afresh, which is exactly
 * what every caller did before the cache existed. */
#define ATLAS_DB_STMT_CACHE 128u

typedef struct atlas_db_cached_stmt {
    /* The SQL *pointer*, as the fast lookup key: every call site passes a
     * string literal with static storage duration, so the pointer is a stable
     * identity and the common case is a pointer compare. */
    const char *sql;
    /* A copy of the text, owned by the cache, used to confirm a pointer hit.
     *
     * Without it the pointer key is unsound for any caller that formats SQL
     * into a reused buffer: the second call presents the same address with
     * different text and is handed the previous statement, which then executes
     * the wrong query against the right bindings. That is a silent wrong
     * answer, and it happened during A4's correction pass.
     *
     * The copy is what makes the confirmation safe to perform at all — the
     * caller's buffer may since have been freed, so the comparison must be
     * against memory the cache owns. */
    char *sql_owned;
    sqlite3_stmt *stmt;
    /* True while a caller holds it. A statement handed out twice would have its
     * iteration reset mid-flight by the second holder, so a re-entrant use gets
     * a freshly prepared one instead. */
    bool in_use;
} atlas_db_cached_stmt;

struct atlas_db {
    sqlite3 *h;
    atlas_db_caps caps;
    int tx_depth;
    bool fts_ready;  /* the FTS5 shadow tables exist */
    bool read_only;  /* opened SQLITE_OPEN_READONLY; no write may be attempted */
    /* One cache per handle, and a handle belongs to one thread — the same rule
     * the rest of the daemon already keeps, so there is nothing to synchronise. */
    atlas_db_cached_stmt stmt_cache[ATLAS_DB_STMT_CACHE];
    size_t stmt_cache_count;
};

/* Wraps the last sqlite error into `err` with `what` as context. */
atlas_status atlas_db_fail(atlas_db *db, atlas_err *err, atlas_status st, const char *what);

atlas_status atlas_db_exec_sql(atlas_db *db, const char *sql, atlas_err *err);
/* Prepares `sql`, or returns the cached statement for it reset and unbound.
 *
 * `sql` must have static storage duration: the cache is keyed on the pointer.
 * A constructed string simply misses the cache. */
atlas_status atlas_db_prepare(atlas_db *db, const char *sql, sqlite3_stmt **out, atlas_err *err);

/* Returns a statement to the cache, or finalises it when it is not cached.
 *
 * **Every site in src/db calls this instead of `sqlite3_finalize`.** Finalising
 * a cached statement would leave the cache holding a dangling pointer, so the
 * two are not interchangeable. */
void atlas_db_finish(atlas_db *db, sqlite3_stmt *stmt);

/* Finalises everything the cache holds. Called at close, before the sqlite
 * handle goes: an open statement would keep it from closing. */
void atlas_db_cache_clear(atlas_db *db);

/* Step a statement expected to yield no rows, then release it. */
atlas_status atlas_db_step_done(atlas_db *db, sqlite3_stmt *stmt, atlas_err *err);

/* Single-value queries. */
atlas_status atlas_db_query_int64(atlas_db *db, const char *sql, int64_t *out, atlas_err *err);

/* Test hook: makes this handle behave as though the SQLite build had no FTS5, so
 * the degraded search path can be exercised on a build that does have it. There
 * is no way to reach this from the CLI. */
void atlas_db_disable_fts_for_tests(atlas_db *db);

/* Bind helpers that convert size_t to sqlite's int length safely. */
atlas_status atlas_db_bind_blob(atlas_db *db, sqlite3_stmt *stmt, int idx, const void *data,
                                size_t n, atlas_err *err);
atlas_status atlas_db_bind_text_n(atlas_db *db, sqlite3_stmt *stmt, int idx, const char *s,
                                  size_t n, atlas_err *err);
/* Binds `s`, or SQL NULL when `s` is NULL. */
atlas_status atlas_db_bind_text_opt(atlas_db *db, sqlite3_stmt *stmt, int idx, const char *s,
                                   atlas_err *err);

/* Column accessors that never return NULL for text. */
const char *atlas_db_col_text(sqlite3_stmt *stmt, int col);
const char *atlas_db_col_text_opt(sqlite3_stmt *stmt, int col);

/* Copy a text column into a fixed-size field, failing when it would truncate. */
atlas_status atlas_db_col_copy(sqlite3_stmt *stmt, int col, char *dst, size_t dst_size,
                               const char *field, atlas_err *err);

/* --- migrations --------------------------------------------------------- */

/* A migration is a NULL-terminated list of statement groups rather than one
 * string: ISO C only guarantees 4095-byte string literals, and one group per
 * table keeps the schema readable. */
typedef struct atlas_migration {
    int version;
    const char *name;
    const char *const *statements;
    /* A9.1. Run this migration with `PRAGMA foreign_keys=OFF`, restored
     * afterwards on every exit path.
     *
     * Set on exactly one migration, and it is not a convenience. SQLite cannot
     * widen a CHECK in place, so a migration that adds a member to a stored
     * vocabulary must rebuild the table — and with foreign keys enforced, the
     * implicit DELETE that `DROP TABLE` performs on a table other tables
     * reference does one of two things, both wrong: it fails outright, or, where
     * a child declares `ON DELETE CASCADE`, it silently empties the child.
     * `decision_links.revision_id` declares exactly that, so rebuilding
     * `decision_revisions` with foreign keys on would have deleted every link
     * of every decision without failing.
     *
     * `PRAGMA defer_foreign_keys` does not help: it defers the violations the
     * implicit delete counts and nothing decrements them, so the COMMIT fails
     * even after the rows are back. Measured, not assumed.
     *
     * The pragma is issued before `BEGIN`, because inside a transaction it is a
     * no-op. The migration is still a single transaction, so a failure still
     * leaves the schema version and every row exactly as they were, and the
     * rebuild verifies its own row preservation before that transaction
     * commits. */
    bool foreign_keys_off;
} atlas_migration;

const atlas_migration *atlas_migrations(size_t *count_out);

/* Applies `list` on top of whatever is already recorded. Each migration runs in
 * its own transaction and is rolled back completely if any statement fails.
 * Exposed for tests, which inject a deliberately failing migration. */
atlas_status atlas_db_migrate_list(atlas_db *db, const atlas_migration *list, size_t count,
                                   atlas_err *err);

#endif /* ATLAS_DB_INTERNAL_H */

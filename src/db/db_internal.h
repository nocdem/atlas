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

struct atlas_db {
    sqlite3 *h;
    atlas_db_caps caps;
    int tx_depth;
    bool fts_ready; /* the FTS5 shadow tables exist */
};

/* Wraps the last sqlite error into `err` with `what` as context. */
atlas_status atlas_db_fail(atlas_db *db, atlas_err *err, atlas_status st, const char *what);

atlas_status atlas_db_exec_sql(atlas_db *db, const char *sql, atlas_err *err);
atlas_status atlas_db_prepare(atlas_db *db, const char *sql, sqlite3_stmt **out, atlas_err *err);

/* Step a statement expected to yield no rows, then finalize it. */
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
} atlas_migration;

const atlas_migration *atlas_migrations(size_t *count_out);

/* Applies `list` on top of whatever is already recorded. Each migration runs in
 * its own transaction and is rolled back completely if any statement fails.
 * Exposed for tests, which inject a deliberately failing migration. */
atlas_status atlas_db_migrate_list(atlas_db *db, const atlas_migration *list, size_t count,
                                   atlas_err *err);

#endif /* ATLAS_DB_INTERNAL_H */

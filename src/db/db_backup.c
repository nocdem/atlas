/* Atlas - the SQLite half of backup, verification and restore.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Everything here touches sqlite3 types, which is why it lives in src/db. The
 * filesystem work — temporary files, fsync, atomic publication, symlink
 * refusal — is in src/core/service_backup.c and touches no sqlite3 type.
 */
#include <stdio.h>
#include <string.h>

#include "atlas/backup.h"
#include "atlas/limits.h"
#include "db_internal.h"

/* Tables a file must have to be an Atlas database rather than some other
 * SQLite file. Recognition is deliberately a schema question and not a magic
 * number in a header: Atlas writes a plain SQLite database, and a plain SQLite
 * database is what a restore has to be handed.
 *
 * `since` is the schema version that introduced the table, so a backup from an
 * older supported schema is recognised rather than rejected for lacking tables
 * it could not have had. */
typedef struct atlas_backup_table {
    const char *name;
    int since;
} atlas_backup_table;

static const atlas_backup_table REQUIRED_TABLES[] = {
    {"schema_migrations", 1}, {"repositories", 1},        {"files", 1},
    {"commits", 1},           {"evidence", 1},            {"repo_index_state", 3},
    {"repo_events", 3},       {"ai_sessions", 4},         {"ai_reasons", 4},
    {"code_files", 5},        {"code_symbols", 5},        {"code_relations", 5},
    {"decision_documents", 6}, {"decision_revisions", 6}, {"decision_events", 6},
    /* A8. Verification covers the orchestration tables for the reason it covers
     * the decision tables: they are canonical, nothing rebuilds them, and a
     * backup that restored without them would restore an Atlas that has
     * forgotten every job it ever ran while reporting itself intact. */
    {"orch_jobs", 8},          {"orch_attempts", 8},      {"orch_transitions", 8},
    {"orch_leases", 8},
};

/* --- the online copy ----------------------------------------------------- */

atlas_status atlas_db_backup_copy(atlas_db *src, const char *dest_path, int64_t *pages_out,
                                  int64_t *page_size_out, atlas_err *err) {
    if (pages_out != NULL) {
        *pages_out = 0;
    }
    if (page_size_out != NULL) {
        *page_size_out = 0;
    }
    sqlite3 *dest = NULL;
    /* No SQLITE_OPEN_CREATE: the caller already created the file with mode
     * 0600, and letting SQLite create it would give it 0666 & ~umask instead.
     * A backup that is world-readable because the operator's umask was
     * permissive is a hole nobody would look for. */
    if (sqlite3_open_v2(dest_path, &dest, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        atlas_status st = atlas_err_set(err, ATLAS_ERR_DB, "cannot open backup destination: %s",
                                        dest == NULL ? "out of memory" : sqlite3_errmsg(dest));
        sqlite3_close(dest);
        return st;
    }

    sqlite3_backup *bk = sqlite3_backup_init(dest, "main", src->h, "main");
    if (bk == NULL) {
        atlas_status st =
            atlas_err_set(err, ATLAS_ERR_DB, "cannot start backup: %s", sqlite3_errmsg(dest));
        sqlite3_close(dest);
        return st;
    }

    /* One step for the whole database. Stepping incrementally would let a
     * writer commit between steps, and SQLite would then restart the copy from
     * the beginning — on a busy daemon that is a loop with no bound. A single
     * step holds one read transaction for the duration, so the destination is
     * the source as of exactly one commit boundary, and writers still proceed. */
    int rc = sqlite3_backup_step(bk, -1);
    int64_t pages = sqlite3_backup_pagecount(bk);
    int fin = sqlite3_backup_finish(bk);

    atlas_status st = ATLAS_OK;
    if (rc != SQLITE_DONE) {
        st = atlas_err_set(err, ATLAS_ERR_DB, "backup copy did not complete: %s",
                           sqlite3_errstr(rc));
    } else if (fin != SQLITE_OK) {
        st = atlas_err_set(err, ATLAS_ERR_DB, "backup copy could not be committed: %s",
                           sqlite3_errstr(fin));
    }

    if (st == ATLAS_OK) {
        /* The copy inherits the source's journal mode, and the source is WAL.
         * A WAL-mode file cannot be read without creating a `-shm` beside it,
         * which would make `backup verify` a writer. Switching the finished
         * copy to rollback journalling makes the backup exactly one file. */
        char *msg = NULL;
        if (sqlite3_exec(dest, "PRAGMA journal_mode=DELETE;", NULL, NULL, &msg) != SQLITE_OK) {
            st = atlas_err_set(err, ATLAS_ERR_DB, "cannot make backup self-contained: %s",
                               msg == NULL ? "unknown" : msg);
        }
        sqlite3_free(msg);
    }

    if (st == ATLAS_OK && page_size_out != NULL) {
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(dest, "PRAGMA page_size;", -1, &stmt, NULL) == SQLITE_OK &&
            sqlite3_step(stmt) == SQLITE_ROW) {
            *page_size_out = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    if (pages_out != NULL) {
        *pages_out = pages;
    }

    if (sqlite3_close(dest) != SQLITE_OK && st == ATLAS_OK) {
        st = atlas_err_set(err, ATLAS_ERR_DB, "cannot close backup destination");
    }
    return st;
}

/* --- inspection ---------------------------------------------------------- */

static atlas_status note(atlas_backup_verify_report *r, atlas_err *err, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static atlas_status note(atlas_backup_verify_report *r, atlas_err *err, const char *fmt, ...) {
    if (r->problems.len > 0) {
        atlas_status st = atlas_buf_append_ch(&r->problems, '\n', err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    va_list ap;
    va_start(ap, fmt);
    char line[ATLAS_ERR_MSG_MAX];
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "cannot format backup finding");
    }
    return atlas_buf_append_str(&r->problems, line, err);
}

/* Collect a pragma that reports "ok" on success and one row per problem on
 * failure. Mirrors src/db/db.c, but against a bare handle: the backup is not an
 * `atlas_db` and must never be opened as one, because opening it as one would
 * migrate it. */
static atlas_status pragma_first_row(sqlite3 *h, const char *sql, atlas_buf *out, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(h, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return atlas_err_set(err, ATLAS_ERR_DB, "cannot run %s: %s", sql, sqlite3_errmsg(h));
    }
    atlas_status st = atlas_buf_set_str(out, "ok", err);
    int rc = sqlite3_step(stmt);
    if (st == ATLAS_OK && rc == SQLITE_ROW) {
        const unsigned char *txt = sqlite3_column_text(stmt, 0);
        if (txt != NULL) {
            st = atlas_buf_set_str(out, (const char *)txt, err);
        }
    } else if (st == ATLAS_OK && rc != SQLITE_DONE) {
        st = atlas_err_set(err, ATLAS_ERR_DB, "%s failed: %s", sql, sqlite3_errmsg(h));
    }
    sqlite3_finalize(stmt);
    return st;
}

static bool table_exists(sqlite3 *h, const char *name) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(h, "SELECT 1 FROM sqlite_schema WHERE type='table' AND name=?1;", -1,
                           &stmt, NULL) != SQLITE_OK) {
        return false;
    }
    bool found = false;
    if (sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC) == SQLITE_OK) {
        found = sqlite3_step(stmt) == SQLITE_ROW;
    }
    sqlite3_finalize(stmt);
    return found;
}

static bool scalar_int(sqlite3 *h, const char *sql, int64_t *out) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(h, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return false;
    }
    bool got = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        *out = sqlite3_column_int64(stmt, 0);
        got = true;
    }
    sqlite3_finalize(stmt);
    return got;
}

/* The SQLite header says how many pages of what size the file has, and that
 * product must be the file's length.
 *
 * This is checked before anything else because `PRAGMA integrity_check` does
 * not reliably catch a truncation: it walks the pages the b-trees reach, so a
 * file cut short in unallocated space, or by less than a page, can pass. A
 * backup missing its tail is exactly the failure an operator has, and "it
 * verified" is exactly the wrong answer. */
static atlas_status check_length(const char *path, int64_t observed,
                                 atlas_backup_verify_report *out, atlas_err *err, bool *fatal) {
    *fatal = false;
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG, "cannot read \"%s\"", path);
    }
    unsigned char head[32];
    size_t n = fread(head, 1u, sizeof head, f);
    (void)fclose(f);
    if (n != sizeof head) {
        *fatal = true;
        out->verdict = ATLAS_BACKUP_NOT_SQLITE;
        return note(out, err, "the file is shorter than a SQLite header");
    }
    /* Big-endian, per the SQLite file format: page size at 16, page count at
     * 28. A page size of 1 means 65536, which is the format's way of fitting
     * the largest page into two bytes. */
    int64_t page_size = ((int64_t)head[16] << 8) | (int64_t)head[17];
    if (page_size == 1) {
        page_size = 65536;
    }
    int64_t pages = ((int64_t)head[28] << 24) | ((int64_t)head[29] << 16) |
                    ((int64_t)head[30] << 8) | (int64_t)head[31];
    if (page_size < 512 || (page_size & (page_size - 1)) != 0) {
        *fatal = true;
        out->verdict = ATLAS_BACKUP_NOT_SQLITE;
        return note(out, err, "the SQLite header declares an impossible page size");
    }
    if (pages <= 0) {
        /* Legacy databases leave the count zero and imply it from the length.
         * Nothing Atlas writes does, but refusing a file for a field this build
         * chose not to trust would be an invention. */
        return ATLAS_OK;
    }
    int64_t expected = page_size * pages;
    if (expected != observed) {
        *fatal = true;
        out->verdict = ATLAS_BACKUP_NOT_SQLITE;
        return note(out, err,
                    "the file is %lld bytes but its header describes %lld (%lld pages of %lld); "
                    "it is truncated or was appended to",
                    (long long)observed, (long long)expected, (long long)pages,
                    (long long)page_size);
    }
    return ATLAS_OK;
}

atlas_status atlas_db_backup_inspect(const char *path, atlas_backup_verify_report *out,
                                     atlas_err *err) {
    out->expected_schema_version = ATLAS_SCHEMA_VERSION;
    out->schema_version = -1;

    bool fatal = false;
    atlas_status lst = check_length(path, out->size_bytes, out, err, &fatal);
    if (lst != ATLAS_OK || fatal) {
        return lst;
    }

    sqlite3 *h = NULL;
    /* Read-only and nothing else. No CREATE, no read-write fallback, and no
     * `atlas_db_open`: that path migrates, and a diagnostic that upgrades the
     * artefact it was asked about has destroyed the evidence. */
    if (sqlite3_open_v2(path, &h, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        atlas_status st = note(out, err, "cannot open as a SQLite database: %s",
                               h == NULL ? "out of memory" : sqlite3_errmsg(h));
        sqlite3_close(h);
        out->verdict = ATLAS_BACKUP_NOT_SQLITE;
        return st;
    }

    atlas_status st = ATLAS_OK;

    /* integrity_check first. Everything after it reads rows, and reading rows
     * out of a damaged file is how a checker reports a confident wrong answer. */
    st = pragma_first_row(h, "PRAGMA integrity_check;", &out->integrity, err);
    if (st != ATLAS_OK) {
        out->verdict = ATLAS_BACKUP_NOT_SQLITE;
        st = note(out, err, "the file could not be read as a SQLite database");
        sqlite3_close(h);
        return st;
    }
    if (strcmp(atlas_buf_cstr(&out->integrity), "ok") != 0) {
        out->verdict = ATLAS_BACKUP_CORRUPT;
        st = note(out, err, "sqlite integrity_check did not report ok");
        sqlite3_close(h);
        return st;
    }

    /* Recognition. A structurally valid SQLite file that is not an Atlas index
     * must be refused here rather than restored into a data directory, where it
     * would be migrated and become one. */
    int64_t required = 0, present = 0;
    bool have_migrations = table_exists(h, "schema_migrations");
    if (have_migrations) {
        int64_t v = 0;
        if (scalar_int(h, "SELECT coalesce(max(version), 0) FROM schema_migrations;", &v)) {
            out->schema_version = (int)v;
        }
    }
    if (out->schema_version <= 0) {
        out->verdict = ATLAS_BACKUP_NOT_ATLAS;
        st = note(out, err, "no Atlas schema record: this is not an Atlas database");
        sqlite3_close(h);
        return st;
    }

    for (size_t i = 0; i < sizeof REQUIRED_TABLES / sizeof REQUIRED_TABLES[0]; i++) {
        if (REQUIRED_TABLES[i].since > out->schema_version) {
            continue;
        }
        required++;
        if (table_exists(h, REQUIRED_TABLES[i].name)) {
            present++;
            continue;
        }
        if (st == ATLAS_OK && out->missing_tables.len > 0) {
            st = atlas_buf_append_str(&out->missing_tables, ",", err);
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_append_str(&out->missing_tables, REQUIRED_TABLES[i].name, err);
        }
    }
    out->tables_required = required;
    out->tables_present = present;
    if (st == ATLAS_OK && present != required) {
        out->verdict = ATLAS_BACKUP_NOT_ATLAS;
        st = note(out, err, "schema %d is missing required tables: %s", out->schema_version,
                  atlas_buf_cstr(&out->missing_tables));
        sqlite3_close(h);
        return st;
    }
    if (st != ATLAS_OK) {
        sqlite3_close(h);
        return st;
    }

    /* A newer Atlas wrote it. Refused, and refused *before* anything is
     * touched: migrations only go forward, so a future schema cannot be made
     * into a current one, and pretending otherwise would silently discard
     * columns this build cannot see. */
    if (out->schema_version > ATLAS_SCHEMA_VERSION) {
        out->verdict = ATLAS_BACKUP_SCHEMA_FUTURE;
        st = note(out, err, "schema %d was written by a newer Atlas; this build supports %d",
                  out->schema_version, ATLAS_SCHEMA_VERSION);
        sqlite3_close(h);
        return st;
    }

    /* foreign_key_check is not run by the pragma-collect helper's "ok" idiom:
     * it reports *no* rows for a healthy database rather than one row saying
     * ok, so an empty result is the success case. */
    {
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(h, "PRAGMA foreign_key_check;", -1, &stmt, NULL) != SQLITE_OK) {
            st = atlas_err_set(err, ATLAS_ERR_DB, "cannot run foreign_key_check: %s",
                               sqlite3_errmsg(h));
        } else {
            int rc = sqlite3_step(stmt);
            if (rc == SQLITE_ROW) {
                const unsigned char *tbl = sqlite3_column_text(stmt, 0);
                st = atlas_buf_set_str(&out->foreign_key_check,
                                       tbl == NULL ? "violation" : (const char *)tbl, err);
                if (st == ATLAS_OK) {
                    out->verdict = ATLAS_BACKUP_CORRUPT;
                    st = note(out, err, "sqlite foreign_key_check reported violations");
                }
            } else if (rc == SQLITE_DONE) {
                st = atlas_buf_set_str(&out->foreign_key_check, "ok", err);
            } else {
                st = atlas_err_set(err, ATLAS_ERR_DB, "foreign_key_check failed: %s",
                                   sqlite3_errmsg(h));
            }
            sqlite3_finalize(stmt);
        }
        if (st != ATLAS_OK || out->verdict == ATLAS_BACKUP_CORRUPT) {
            sqlite3_close(h);
            return st;
        }
    }

    (void)scalar_int(h, "SELECT count(*) FROM repositories;", &out->repo_count);
    sqlite3_close(h);
    h = NULL;

    /* A4. The remaining checks need the typed decision operations, so they run
     * against an `atlas_db` — opened read-only, which cannot migrate. */
    atlas_db *db = NULL;
    st = atlas_db_open_readonly(path, &db, err);
    if (st != ATLAS_OK) {
        out->verdict = ATLAS_BACKUP_CORRUPT;
        return note(out, err, "the database could not be reopened for record checks");
    }
    int64_t checked = 0, mismatched = 0, rehashed = 0, corrupt = 0;
    st = atlas_db_decision_verify_all(db, &checked, &mismatched, &rehashed, &corrupt, err);
    atlas_db_close(db);
    if (st != ATLAS_OK) {
        out->verdict = ATLAS_BACKUP_CORRUPT;
        return st;
    }
    out->ledger_mismatched = mismatched;
    out->revisions_checked = checked;
    out->revisions_rehashed = rehashed;
    out->revisions_corrupt = corrupt;

    if (corrupt > 0) {
        out->verdict = ATLAS_BACKUP_INCONSISTENT;
        st = note(out, err,
                  "%lld decision revision(s) no longer hash to their recorded content_hash",
                  (long long)corrupt);
    }
    if (st == ATLAS_OK && mismatched > 0) {
        out->verdict = ATLAS_BACKUP_INCONSISTENT;
        st = note(out, err, "%lld decision document(s) disagree with the lifecycle ledger",
                  (long long)mismatched);
    }
    if (st == ATLAS_OK && out->verdict == ATLAS_BACKUP_OK) {
        out->ok = true;
    }
    return st;
}

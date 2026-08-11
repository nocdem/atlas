/* Atlas - operational database backup, verification and restore.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * What a backup is
 * ----------------
 * One self-contained SQLite database file produced through SQLite's online
 * backup API from a read-only connection to the live index. It is not a copy of
 * `atlas.db`, `atlas.db-wal` and `atlas.db-shm` as ordinary files: those three
 * are only meaningful together and only at an instant no reader can name, so
 * copying them while a daemon writes produces a file that opens and is wrong.
 *
 * The copy runs inside one read transaction on the source connection, so the
 * result is the database as of a single commit boundary, including everything
 * that was still in the write-ahead log. Writers are not blocked while it runs.
 *
 * Before publication the copy is switched to rollback journalling, which makes
 * it a single file with no sidecars. That is what lets `atlas backup verify`
 * open it read-only and create nothing at all — a WAL-mode file would require a
 * `-shm` to be created just to be read, and a verification that writes is not a
 * verification.
 *
 * What is not in a backup
 * -----------------------
 * A backup contains the database and nothing else. Configuration, the runtime
 * socket, the writer lock, systemd units and the Claude integration files are
 * not database content and are not captured, restored or implied by one.
 *
 * A backup is **not encrypted and not signed**. It is a plain SQLite file with
 * whatever protection its directory and mode give it; Atlas creates it 0600 and
 * makes no cryptographic claim about it. The recorded SHA-256 detects damage
 * and accident. It is not a signature and establishes nothing about who wrote
 * the file.
 *
 * Where this lives
 * ----------------
 * Backup, verification and restore are **local CLI operations only**. They are
 * reachable from `atlas backup`, from nothing else, and deliberately: there is
 * no IPC method, no MCP tool and no hook that can create, read or restore one.
 * A model that can call every method Atlas exposes still cannot replace the
 * index. The absence is structural — no method exists — rather than a check
 * that could be forgotten.
 */
#ifndef ATLAS_BACKUP_H
#define ATLAS_BACKUP_H

#include <stdbool.h>
#include <stdint.h>

#include "atlas/buf.h"
#include "atlas/db.h"
#include "atlas/error.h"
#include "atlas/sha256.h"

/* Why a backup file was rejected. `ATLAS_BACKUP_OK` is the only verdict under
 * which a restore may proceed.
 *
 * The distinctions are the ones an operator needs in order to act: a truncated
 * download and a database from a newer Atlas are both "unusable", but only one
 * of them is fixed by fetching the file again. */
typedef enum atlas_backup_verdict {
    ATLAS_BACKUP_OK = 0,
    ATLAS_BACKUP_UNREADABLE,     /* missing, not a regular file, or empty */
    ATLAS_BACKUP_NOT_SQLITE,     /* no SQLite header; truncation shows up here too */
    ATLAS_BACKUP_NOT_ATLAS,      /* a valid SQLite database that Atlas did not write */
    ATLAS_BACKUP_SCHEMA_FUTURE,  /* written by a newer Atlas; never restored */
    ATLAS_BACKUP_CORRUPT,        /* integrity_check or foreign_key_check failed */
    ATLAS_BACKUP_INCONSISTENT    /* structurally sound, but its own records disagree */
} atlas_backup_verdict;

const char *atlas_backup_verdict_name(atlas_backup_verdict v);

/* Parses a verdict name back. False for a name this binary does not know, which
 * is a client/daemon version mismatch rather than a bad backup — the caller
 * must be able to tell that apart from a verdict of `ok`. */
bool atlas_backup_verdict_parse(const char *name, atlas_backup_verdict *out);

/* --- create -------------------------------------------------------------- */

typedef struct atlas_backup_report {
    atlas_buf path;           /* the published file, absolute */
    atlas_buf source_db_path; /* the database it was taken from */
    int64_t size_bytes;
    char sha256[ATLAS_SHA256_HEX_LEN + 1u];
    int schema_version;
    char atlas_version[32];
    int64_t page_size;
    int64_t page_count;
    /* True when another process held the writer lock while the copy ran, that
     * is, when this was an online snapshot of a live daemon rather than a copy
     * of a quiescent data directory. Reported because the two are operationally
     * different situations, not because the result differs. */
    bool source_online;
} atlas_backup_report;

void atlas_backup_report_init(atlas_backup_report *r);
void atlas_backup_report_free(atlas_backup_report *r);

/* --- verify -------------------------------------------------------------- */

typedef struct atlas_backup_verify_report {
    atlas_buf path;
    atlas_backup_verdict verdict;
    bool ok;
    int64_t size_bytes;
    char sha256[ATLAS_SHA256_HEX_LEN + 1u];
    int schema_version;          /* -1 when it could not be read */
    int expected_schema_version;
    atlas_buf integrity;         /* "ok", or the first problem SQLite reported */
    atlas_buf foreign_key_check;
    int64_t tables_required;
    int64_t tables_present;
    atlas_buf missing_tables;    /* comma-separated, empty when none */
    int64_t repo_count;
    /* A4: the immutability claim, rechecked rather than assumed. Every revision
     * is rehashed from its stored content and compared with its recorded
     * `content_hash`, and every document's cached status is recomputed from the
     * append-only ledger. Reported, never repaired. */
    int64_t revisions_checked;
    int64_t revisions_rehashed;
    int64_t revisions_corrupt;
    int64_t ledger_mismatched;
    atlas_buf problems;          /* newline-separated, empty when ok */
} atlas_backup_verify_report;

void atlas_backup_verify_report_init(atlas_backup_verify_report *r);
void atlas_backup_verify_report_free(atlas_backup_verify_report *r);

/* --- restore ------------------------------------------------------------- */

typedef struct atlas_backup_restore_report {
    atlas_buf data_dir;
    atlas_buf db_path;
    /* The consistent snapshot taken of whatever database was already there,
     * before it was replaced. Empty when the data directory held none. It is
     * left in place: Atlas never deletes the thing it displaced. */
    atlas_buf recovery_path;
    bool recovery_made;
    /* The staged copy was published as the index. True on every successful
     * restore, including into an empty data directory: it says the file became
     * the index, not that anything was displaced. `recovery_made` is what says
     * something was. The two were one field called `replaced` and it meant
     * whichever of the two the reader assumed. */
    bool published;
    /* Sidecars belonging to the replaced database that were removed so they
     * could not be applied to the restored one. */
    bool removed_wal;
    bool removed_shm;
    bool migrated;
    int schema_before; /* -1 when there was no database */
    int schema_after;
    atlas_backup_verify_report source;    /* the backup, checked before anything moved */
    atlas_backup_verify_report installed; /* the database, rechecked in place */
} atlas_backup_restore_report;

void atlas_backup_restore_report_init(atlas_backup_restore_report *r);
void atlas_backup_restore_report_free(atlas_backup_restore_report *r);

/* --- operations ---------------------------------------------------------- */

typedef struct atlas_backup_create_opts {
    const char *output; /* absolute, or relative to the current directory */
    bool force;         /* replace an existing destination */
} atlas_backup_create_opts;

/* Writes a snapshot of the index in `data_dir_override` (or the resolved
 * default) to `opts->output`.
 *
 * It takes a data directory rather than an `atlas_ctx` because it must not have
 * one: a context in any writing mode competes for the writer lock, and taking
 * the lock is exactly what a backup must not do. The source is opened
 * read-only, so a running daemon keeps the lock and keeps writing while the
 * snapshot is taken.
 *
 * The destination's parent is resolved component by component without ever
 * traversing a symlink; the copy is written to a mode-0600 temporary file in
 * that directory, verified in full, fsynced, and published by rename. A failure
 * at any point leaves no published file. */
atlas_status atlas_service_backup_create(const char *data_dir_override,
                                         const atlas_backup_create_opts *opts,
                                         atlas_backup_report *out, atlas_err *err);

/* Reads `path` and reports on it. Creates nothing, writes nothing, repairs
 * nothing, and needs no data directory: it can be run on a machine where Atlas
 * has never stored anything.
 *
 * Returns ATLAS_OK with `out->ok == false` for a backup that is merely bad —
 * being told a file is corrupt is the answer, not an error. A non-OK status
 * means the question could not be asked. */
atlas_status atlas_service_backup_verify(const char *path, atlas_backup_verify_report *out,
                                         atlas_err *err);

typedef struct atlas_backup_restore_opts {
    const char *input;
    bool confirmed; /* --yes; a restore replaces the index and is refused without it */
} atlas_backup_restore_opts;

/* Replaces the index in `data_dir_override` (or the resolved default) with
 * `opts->input`.
 *
 * It acquires the data-directory writer lock exclusively for the whole
 * operation, which is what proves no daemon is running: that is the same lock
 * the daemon holds for its entire lifetime, so a running daemon makes this
 * refuse rather than race.
 *
 * The order is: verify the backup completely, refuse every symlinked
 * destination component, snapshot the existing database, stage a copy beside
 * it, and only then publish. Everything that can fail, fails with the original
 * database byte-identical. */
atlas_status atlas_service_backup_restore(const char *data_dir_override,
                                          const atlas_backup_restore_opts *opts,
                                          atlas_backup_restore_report *out, atlas_err *err);

/* --- internals shared with src/db ---------------------------------------- */

/* Copies the database `src` is connected to into `dest_path`, which must
 * already exist as an empty regular file. `src` may be a read-only connection
 * and normally is. */
atlas_status atlas_db_backup_copy(atlas_db *src, const char *dest_path, int64_t *pages_out,
                                  int64_t *page_size_out, atlas_err *err);

/* Opens `path` read-only and fills in everything `backup verify` reports about
 * the database itself. The caller has already stat'd and hashed the file. */
atlas_status atlas_db_backup_inspect(const char *path, atlas_backup_verify_report *out,
                                     atlas_err *err);

#endif /* ATLAS_BACKUP_H */

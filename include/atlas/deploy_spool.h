/* Atlas - A17 T2: the queue-file spool and the `.res` parser.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * D.3 describes two directories under the data directory,
 * `<data-dir>/deploy/{requests,results}`, and two byte-for-byte file formats.
 * This file is the whole of what reads or writes them: the daemon composes
 * every path itself from the data directory and a deploy uid this process
 * already validated the shape of -- never from anything a request names, so
 * there is no path here a caller controls.
 *
 * The root agent (`deploy/a17/atlas-deploy-agent.sh`, another task's file) is
 * the only thing on the other end of these directories, and it opens no
 * database. Everything below reads and writes plain files; nothing here
 * forks a process, and nothing here is reached from the daemon socket
 * directly -- `src/daemon/writer.c`'s `ATLAS_JOB_DEPLOY`, `_SPOOL` and
 * `_INGEST` job bodies are the only callers.
 */
#ifndef ATLAS_DEPLOY_SPOOL_H
#define ATLAS_DEPLOY_SPOOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "atlas/db.h"
#include "atlas/deploy.h"
#include "atlas/error.h"

/* The watcher tick's own cadence for asking "is `results/` non-empty?" --
 * short, because D.3's test case (f) waits on the daemon noticing a result
 * written while it is already running, and because the check itself is one
 * bounded `opendir`/`readdir`/`closedir`, cheap enough to repeat often. Not
 * `ATLAS_WATCH_RECONCILE_INTERVAL_MS` (5 minutes): that interval answers "how
 * long may a repository's tree go unexamined", an unrelated question this
 * one must not inherit the answer to. */
#define ATLAS_DEPLOY_INGEST_SWEEP_INTERVAL_MS 2000

/* A `.res` file's total size on disk is bounded well above
 * `ATLAS_DEPLOY_RESULT_TEXT_MAX` (32 KiB): the eleven header lines add a few
 * hundred bytes at most, and this bound exists only so a read never has to
 * guess how large a buffer to allocate for a file that turns out to be
 * something else entirely. A file larger than this is malformed without its
 * bytes ever being examined.
 *
 * Both fixed-size stack buffers this bound governs, in `src/daemon/deploy_spool.c`,
 * are deliberate: `ingest_one_file`'s per-file read buffer is
 * `ATLAS_DEPLOY_RESULT_FILE_MAX + 1` bytes (36 KiB), and `atlas_deploy_ingest_pass`'s
 * sorted filename list is `ATLAS_DEPLOY_INGEST_MAX_FILES * (ATLAS_DEPLOY_UID_MAX + 8)`
 * = 256 * 48 = 12 KiB. Both are well inside an ordinary thread's stack, sized
 * from this bound and `ATLAS_DEPLOY_INGEST_MAX_FILES` rather than guessed, and
 * neither grows with the number of `.res` files a hostile or merely prolific
 * root agent could ever write: the ingest pass reads one bounded buffer at a
 * time and refuses (never truncates) anything past either limit. */
#define ATLAS_DEPLOY_RESULT_FILE_MAX (ATLAS_DEPLOY_RESULT_TEXT_MAX + 4096u)

/* Creates `<data_dir>/deploy`, `<data_dir>/deploy/requests` and
 * `<data_dir>/deploy/results`, mode 0700, for any that is absent. Idempotent:
 * an already-existing directory -- even one an operator or a prior daemon run
 * created -- is left exactly as it is. Called once at daemon startup, before
 * the watcher's first tick can ask whether `results/` holds anything and
 * before any deploy can be confirmed. */
atlas_status atlas_deploy_spool_ensure_dirs(const char *data_dir, atlas_err *err);

/* Pure read, no side effect, safe to call every watcher tick: true when
 * `<data_dir>/deploy/results` holds at least one entry matching exactly `d`
 * plus 32 lowercase hex plus `.res` -- never a `.tmp.*` fragment the agent's
 * tmp-then-rename write leaves mid-write, and never a stray `.bad`. Reads
 * "no such directory" as false rather than an error: a daemon that has not
 * yet run `atlas_deploy_spool_ensure_dirs` (or one whose data directory was
 * hand-edited) owes nothing, not a crash. */
bool atlas_deploy_spool_results_pending(const char *data_dir);

/* Writes the two queue files for one CONFIRMED deploy -- D.3's
 * `<uid>.patch` (the stored `changes.patch` artifact's bytes, verbatim) then
 * `<uid>.req` (the fixed `key value` header), each tmp then rename, `.patch`
 * before `.req` because the root agent's path unit watches `*.req` alone --
 * and, only once both are on disk, `atlas_db_deploy_mark_spooled_in_tx` in a
 * transaction of its own.
 *
 * `db` is the writer's own writable handle. This function opens no
 * transaction around the file I/O: A1 forbids a file read inside a write
 * transaction, and by the same argument a file *write* does not belong
 * inside one either, so every read this function does (the deploy row, the
 * job's stored patch artifact) and every byte it writes happen with no
 * transaction open, and only `mark_spooled_in_tx` itself is wrapped in one.
 *
 * Never surfaces an ordinary spooling failure as an error to `err`: a
 * deploy uid that does not resolve to a CONFIRMED row is not this function's
 * problem to report (the caller decided to spool it), and a filesystem error
 * or a stored-artifact digest that does not match the row's own
 * `patch_digest` (defensive: T1's `propose_in_tx` already checked this once)
 * is logged to `log` (may be NULL) and leaves the row CONFIRMED with
 * `spooled_at` still empty -- D.1's contract, retried later by
 * `atlas_deploy_spool_sweep`. `*spooled_out` reports which happened: true
 * only when both files and the mark-spooled commit all succeeded. `err` is
 * set, and this returns non-OK, only for a database error reading the row or
 * the stored artifact -- a fact worth the caller's attention, unlike an
 * ordinary spool failure. */
atlas_status atlas_deploy_spool_write_one(atlas_db *db, const char *data_dir,
                                          const char *deploy_uid, FILE *log, bool *spooled_out,
                                          atlas_err *err);

/* Re-attempts `atlas_deploy_spool_write_one` for every CONFIRMED deploy whose
 * `spooled_at` is still empty (`atlas_db_deploy_confirmed_unspooled`). Called
 * once at daemon startup and by `ATLAS_JOB_DEPLOY_SPOOL`; safe to call again
 * at any time, since an already-spooled deploy is not in the set it reads. */
atlas_status atlas_deploy_spool_sweep(atlas_db *db, const char *data_dir, FILE *log,
                                      atlas_err *err);

/* One `.res` file's parsed content, borrowed buffers valid only until the
 * next parse call on the same struct -- there is exactly one caller
 * (`atlas_deploy_ingest_pass`), which copies what it needs into an
 * `atlas_deploy_result` immediately. Every string is NUL-terminated and
 * already bounded by its array's size, so a caller never measures one before
 * using it. */
typedef struct atlas_deploy_result_parsed {
    char deploy_uid[ATLAS_DEPLOY_UID_MAX];
    bool success; /* from `outcome SUCCEEDED|FAILED` */
    char stage[128];
    bool dry_run;
    char rollback[64];
    char head_before[64];
    char head_after[64];
    char installed_version[256];
    char text[ATLAS_DEPLOY_RESULT_TEXT_MAX + 1u];
    size_t text_len;
} atlas_deploy_result_parsed;

/* Parses D.3's `.res` grammar from `bytes[0..len)`, bounded throughout.
 *
 * Four keys are required -- `deploy`, `outcome`, `stage`, `text_bytes` -- and
 * seven are optional, defaulting to `""` (`no` for the two yes/no fields):
 * D.3's own escape hatch has root write exactly `outcome FAILED`,
 * `stage ABANDONED` and a short text by hand, and requiring all eleven header
 * lines would turn that hand-written line into just another malformed
 * result. Every key, required or not, may appear at most once; an unknown
 * key anywhere refuses the whole file. The header ends at a line that is
 * exactly `--`; everything after it to EOF is the body, and its length must
 * equal `text_bytes` exactly.
 *
 * On success returns ATLAS_OK and `*ok_out` true. On a malformed file returns
 * ATLAS_OK (this is not a database or I/O failure) and `*ok_out` false, with
 * `why` holding a short, safe sentence naming what failed -- the reason
 * `atlas_db_deploy_finish_in_tx`'s FAILED transition records. `err` is set,
 * and this returns non-OK, only for the caller's own precondition (`out`,
 * `ok_out` or `why` NULL). */
atlas_status atlas_deploy_result_parse(const char *bytes, size_t len,
                                       atlas_deploy_result_parsed *out, bool *ok_out, char *why,
                                       size_t why_size, atlas_err *err);

/* Ingests every `<data_dir>/deploy/results`'s `*.res` files currently on disk, in
 * filename order: reads it (bounded by `ATLAS_DEPLOY_RESULT_FILE_MAX`),
 * parses it, moves the deploy the *filename* names from CONFIRMED to
 * SUCCEEDED or FAILED (`atlas_db_deploy_finish_in_tx`, actor DEPLOY_AGENT) --
 * a malformed parse, a `deploy` field that disagrees with the filename, or a
 * `success` the file did not claim all read as FAILED with stage `INGEST` --
 * and unlinks the file once its outcome (of any kind) has committed. A file
 * whose deploy uid does not resolve to a CONFIRMED row (already terminal, or
 * never existed) is unlinked without a write: reprocessing it forever would
 * not make it resolve.
 *
 * `db` is the writer's own writable handle; each file gets its own
 * transaction, never one transaction for the whole directory, so one
 * malformed file cannot roll back another file's legitimate result. */
atlas_status atlas_deploy_ingest_pass(atlas_db *db, const char *data_dir, FILE *log,
                                      atlas_err *err);

#endif /* ATLAS_DEPLOY_SPOOL_H */

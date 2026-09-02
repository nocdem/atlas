/* Atlas - the T8/T11 reconciliation-pass fixture, shared between
 * test_memory_reconcile.c and test_memory_reconcile_live.c.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A12.1 T11 fix round, item 6: T11 labelled `test_memory_reconcile`
 * "integration;daemon" and added `RUN_SERIAL TRUE` because two of its new
 * cases fork a real daemon -- `test_memory_methods_refuse_a_non_operator_peer`
 * and `test_memory_survives_a_daemon_restart`. That cost the other fifty-odd
 * in-process cases in the file both `ctest -LE daemon` and parallelism in
 * every `ctest -j`. Splitting the two daemon cases into their own binary
 * needs this harness in both places, so it is hoisted here rather than
 * copied: one implementation, not two to keep in step.
 *
 * `t8env` builds both halves the reconciliation pass needs: a real git
 * repository for the observe phase to read (a tracked or untracked source
 * file lives under it), and a registered repository row with a matching
 * `commits` row for whatever the apply phase looks up. `t8_policy`,
 * `t8_scalar` and `t8_run_pass` are *not* hoisted here -- they are used only
 * by the T8/T9 pass tests that stay in `test_memory_reconcile.c`, and the
 * daemon cases that move to `test_memory_reconcile_live.c` do not call them.
 */
#ifndef ATLAS_TEST_RECONCILE_ENV_H
#define ATLAS_TEST_RECONCILE_ENV_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "atlas/buf.h"
#include "atlas/db.h"
#include "atlas/error.h"
#include "daemon/daemon_internal.h"
#include "support/fixture.h"

typedef struct t8env {
    fixture fx;
    atlas_buf db_path;
    atlas_db *db;
    int64_t repo_id;
    atlas_repo_info repo;
} t8env;

/* Opens the fixture: a real git repository, a migrated database and one
 * registered repository row ("proj"). */
void t8_env_open(t8env *e, atlas_err *err);

/* Binds the repository's own `scanned_head` (and a matching `commits` row) to
 * the real repository's current HEAD, then refetches `e->repo` so the struct
 * the pass reads matches what was just bound. Called again after every commit
 * that should move what a fresh pass binds to. */
void t8_bind_head(t8env *e, atlas_err *err);

/* A row in `files`, so a backtick token resolves as a PATH anchor and a
 * memory source's own path passes EVIDENCE_ADD's index lookup. The hash is
 * never checked against real content anywhere the reconciliation pass runs. */
void t8_seed_file(t8env *e, const char *path, const char *hash, atlas_err *err);

void t8_env_close(t8env *e);

/* T11. Opens a fresh writer against `e`'s database, logging to `/dev/null`.
 * Every case that drives `atlas_writer_memory_put` or
 * `atlas_writer_submit_memory_reconcile` needs exactly this. */
void t11_writer_open(t8env *e, FILE **log_out, atlas_writer **w_out, atlas_err *err);
void t11_writer_close(FILE *log, atlas_writer *w);

/* T11. Reads a scalar off an arbitrary handle -- `t8_scalar`'s own shape but
 * not tied to a `t8env`, for a case that deliberately holds a handle other
 * than `e->db` (a fresh one, or one opened after the writer closed its own). */
int64_t t11_scalar(atlas_db *db, const char *sql, atlas_err *err);

/* T11. Waits for one memory reconciliation to land a generation, the way a
 * caller would poll `memory.status` rather than a guessed sleep --
 * `fx_wait_for_substring`'s own discipline, one layer down at the database
 * instead of the socket. */
bool t11_wait_for_generation(t8env *e, int64_t *gen_out, atlas_err *err);

#endif /* ATLAS_TEST_RECONCILE_ENV_H */

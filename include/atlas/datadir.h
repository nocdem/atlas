/* Atlas - data directory resolution.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Resolution order:
 *   1. explicit override (--data-dir)
 *   2. ATLAS_DATA_DIR
 *   3. XDG_DATA_HOME/atlas
 *   4. $HOME/.local/share/atlas   (the local data directory on this platform)
 *
 * An empty or relative value in any of these sources is a configuration error,
 * never silently reinterpreted. Directories are created with mode 0700 and the
 * database file with mode 0600.
 */
#ifndef ATLAS_DATADIR_H
#define ATLAS_DATADIR_H

#include <stdbool.h>

#include "atlas/buf.h"
#include "atlas/error.h"

#define ATLAS_DB_FILENAME "atlas.db"

/* Which source supplied the value, for `atlas doctor`. */
typedef enum atlas_datadir_source {
    ATLAS_DATADIR_OVERRIDE = 0,
    ATLAS_DATADIR_ENV,
    ATLAS_DATADIR_XDG,
    ATLAS_DATADIR_HOME,
    /* A7.1: a root-anchored system policy named it. */
    ATLAS_DATADIR_SYSTEM
} atlas_datadir_source;

const char *atlas_datadir_source_name(atlas_datadir_source src);

/* Resolve without touching the filesystem. `override` may be NULL. */
atlas_status atlas_datadir_resolve(const char *override, atlas_buf *out,
                                   atlas_datadir_source *src_out, atlas_err *err);

/* Create `dir` and any missing parents with mode 0700. */
atlas_status atlas_datadir_ensure(const char *dir, atlas_err *err);

/* Append ATLAS_DB_FILENAME to `dir`, writing "<dir>/atlas.db" into `out`. */
atlas_status atlas_datadir_db_path(const char *dir, atlas_buf *out, atlas_err *err);

/* True when `dir` is a system index this process does not own.
 *
 * A7.1 puts the index behind a separate OS principal: `/var/lib/atlas` is 0700
 * `atlasd`, and it has to stay that way because `atlas-worker` is a member of
 * the client group and must not be able to read the index. So a client uid can
 * never open that database directly, whatever Atlas does — the socket is the
 * only read path, and a command that wants a fact has to ask the daemon for it.
 *
 * The test is the *source* plus ownership rather than the path: an explicit
 * `--data-dir` or `ATLAS_DATA_DIR` still means exactly what it says, so a
 * fixture, a test and an ad-hoc per-user daemon behave as they always did on a
 * machine that happens to carry a system policy. */
bool atlas_datadir_is_foreign(const char *dir, atlas_datadir_source src);

#endif /* ATLAS_DATADIR_H */

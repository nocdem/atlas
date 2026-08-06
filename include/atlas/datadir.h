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

#include "atlas/buf.h"
#include "atlas/error.h"

#define ATLAS_DB_FILENAME "atlas.db"

/* Which source supplied the value, for `atlas doctor`. */
typedef enum atlas_datadir_source {
    ATLAS_DATADIR_OVERRIDE = 0,
    ATLAS_DATADIR_ENV,
    ATLAS_DATADIR_XDG,
    ATLAS_DATADIR_HOME
} atlas_datadir_source;

const char *atlas_datadir_source_name(atlas_datadir_source src);

/* Resolve without touching the filesystem. `override` may be NULL. */
atlas_status atlas_datadir_resolve(const char *override, atlas_buf *out,
                                   atlas_datadir_source *src_out, atlas_err *err);

/* Create `dir` and any missing parents with mode 0700. */
atlas_status atlas_datadir_ensure(const char *dir, atlas_err *err);

/* Append ATLAS_DB_FILENAME to `dir`, writing "<dir>/atlas.db" into `out`. */
atlas_status atlas_datadir_db_path(const char *dir, atlas_buf *out, atlas_err *err);

#endif /* ATLAS_DATADIR_H */

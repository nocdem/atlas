/* Atlas - the systemd user service unit.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Atlas generates its own unit rather than shipping a template, because the unit
 * has to name the absolute path of the binary that generated it. A template with
 * a guessed path is the classic way a service silently starts the wrong build.
 *
 * The unit is a *user* unit. Atlas never runs as root, never writes to
 * /etc/systemd, and never invokes sudo — not even to offer to. The index
 * describes a person's repositories and lives in their data directory; a
 * system-wide daemon reading it would be a privilege boundary Atlas has no
 * reason to cross.
 *
 * Installation is a separate, explicit command. `atlas service print` writes the
 * unit to stdout and changes nothing, so the normal review-then-apply workflow
 * is possible without Atlas touching the filesystem at all.
 */
#ifndef ATLAS_UNIT_H
#define ATLAS_UNIT_H

#include <stdbool.h>

#include "atlas/buf.h"
#include "atlas/error.h"

#define ATLAS_UNIT_FILENAME "atlas.service"

/* Renders the unit for `exe_path`, which must be absolute.
 *
 * Refuses a path containing a byte that would need quoting or that systemd would
 * interpret. Escaping it correctly is possible; refusing is better, because a
 * unit file is executed and a subtly mis-escaped ExecStart is a command
 * injection with extra steps. */
atlas_status atlas_unit_render(const char *exe_path, const char *data_dir_override, atlas_buf *out,
                               atlas_err *err);

/* The absolute path of the running executable, resolved through /proc/self/exe.
 * Used so the generated unit names the binary that generated it. */
atlas_status atlas_unit_self_path(atlas_buf *out, atlas_err *err);

/* Where the unit belongs: $XDG_CONFIG_HOME/systemd/user, or
 * $HOME/.config/systemd/user when XDG_CONFIG_HOME is unset. */
atlas_status atlas_unit_dir(atlas_buf *out, atlas_err *err);
atlas_status atlas_unit_path(atlas_buf *out, atlas_err *err);

typedef struct atlas_unit_install_report {
    atlas_buf path;
    atlas_buf dir;
    bool created_dir;
    bool wrote_file;
    bool replaced_existing;
    bool unchanged;      /* the file already held exactly this content */
    bool removed;        /* uninstall only */
    bool was_absent;     /* uninstall only */
    atlas_buf unit_text;
} atlas_unit_install_report;

void atlas_unit_install_report_init(atlas_unit_install_report *r);
void atlas_unit_install_report_free(atlas_unit_install_report *r);

/* Writes the unit atomically with mode 0600.
 *
 * Refuses when the target exists and is not a regular file, when it is a
 * symlink, or when it is a regular file that Atlas did not write — identified by
 * the generated-by marker in the unit text. Overwriting somebody's hand-written
 * unit is not a thing an install command gets to do quietly.
 *
 * Never enables and never starts the service. A command that installs a unit and
 * also starts it has made a decision the user did not ask for. */
atlas_status atlas_unit_install(const char *exe_path, const char *data_dir_override, bool force,
                                atlas_unit_install_report *out, atlas_err *err);

/* Removes the unit, and only if Atlas wrote it. */
atlas_status atlas_unit_uninstall(bool force, atlas_unit_install_report *out, atlas_err *err);

#endif /* ATLAS_UNIT_H */

/* Atlas - data directory resolution and creation.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 */
#include "atlas/datadir.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

const char *atlas_datadir_source_name(atlas_datadir_source src) {
    switch (src) {
    case ATLAS_DATADIR_OVERRIDE: return "--data-dir";
    case ATLAS_DATADIR_ENV: return "ATLAS_DATA_DIR";
    case ATLAS_DATADIR_XDG: return "XDG_DATA_HOME";
    case ATLAS_DATADIR_HOME: return "HOME";
    }
    return "unknown";
}

/* A configured path must be usable exactly as given: an empty or relative value
 * is a configuration error rather than something to guess at. */
static atlas_status require_absolute(const char *value, const char *source, atlas_err *err) {
    if (value[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_CONFIG, "%s is set but empty", source);
    }
    if (value[0] != '/') {
        return atlas_err_set(err, ATLAS_ERR_CONFIG,
                             "%s must be an absolute path, got \"%s\"", source, value);
    }
    return ATLAS_OK;
}

/* Removes trailing slashes so that "/x/" and "/x" resolve to the same place. */
static atlas_status append_trimmed(atlas_buf *out, const char *value, atlas_err *err) {
    size_t n = strlen(value);
    while (n > 1u && value[n - 1u] == '/') {
        n--;
    }
    return atlas_buf_append(out, value, n, err);
}

atlas_status atlas_datadir_resolve(const char *override, atlas_buf *out,
                                   atlas_datadir_source *src_out, atlas_err *err) {
    atlas_buf_reset(out);
    atlas_datadir_source src = ATLAS_DATADIR_OVERRIDE;
    atlas_status st;

    if (override != NULL) {
        st = require_absolute(override, "--data-dir", err);
        if (st != ATLAS_OK) {
            return st;
        }
        st = append_trimmed(out, override, err);
        goto done;
    }

    const char *env = getenv("ATLAS_DATA_DIR");
    if (env != NULL) {
        src = ATLAS_DATADIR_ENV;
        st = require_absolute(env, "ATLAS_DATA_DIR", err);
        if (st != ATLAS_OK) {
            return st;
        }
        st = append_trimmed(out, env, err);
        goto done;
    }

    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg != NULL && xdg[0] != '\0') {
        src = ATLAS_DATADIR_XDG;
        st = require_absolute(xdg, "XDG_DATA_HOME", err);
        if (st != ATLAS_OK) {
            return st;
        }
        st = append_trimmed(out, xdg, err);
        if (st == ATLAS_OK) {
            st = atlas_buf_append_str(out, "/atlas", err);
        }
        goto done;
    }

    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_CONFIG,
                             "cannot determine a data directory: set ATLAS_DATA_DIR, "
                             "XDG_DATA_HOME or HOME");
    }
    src = ATLAS_DATADIR_HOME;
    st = require_absolute(home, "HOME", err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = append_trimmed(out, home, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_append_str(out, "/.local/share/atlas", err);
    }

done:
    if (st == ATLAS_OK && src_out != NULL) {
        *src_out = src;
    }
    return st;
}

atlas_status atlas_datadir_ensure(const char *dir, atlas_err *err) {
    if (dir == NULL || dir[0] != '/') {
        return atlas_err_set(err, ATLAS_ERR_CONFIG, "data directory must be an absolute path");
    }
    size_t n = strlen(dir);
    atlas_buf partial = ATLAS_BUF_INIT;
    atlas_status st = ATLAS_OK;

    for (size_t i = 1; i <= n; i++) {
        if (i != n && dir[i] != '/') {
            continue;
        }
        atlas_buf_reset(&partial);
        st = atlas_buf_append(&partial, dir, i, err);
        if (st != ATLAS_OK) {
            break;
        }
        const char *path = atlas_buf_cstr(&partial);
        /* 0700: the index may describe private repositories, so it is never
         * group- or world-accessible. */
        if (mkdir(path, S_IRWXU) != 0 && errno != EEXIST) {
            st = atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno, "cannot create %s", path);
            break;
        }
        struct stat sb;
        if (stat(path, &sb) != 0) {
            st = atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno, "cannot stat %s", path);
            break;
        }
        if (!S_ISDIR(sb.st_mode)) {
            st = atlas_err_set(err, ATLAS_ERR_CONFIG, "%s exists and is not a directory", path);
            break;
        }
    }
    atlas_buf_free(&partial);
    if (st != ATLAS_OK) {
        return st;
    }
    /* Tighten the leaf even when it already existed with looser permissions. */
    if (chmod(dir, S_IRWXU) != 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno,
                                   "cannot restrict permissions on %s", dir);
    }
    return ATLAS_OK;
}

atlas_status atlas_datadir_db_path(const char *dir, atlas_buf *out, atlas_err *err) {
    atlas_buf_reset(out);
    atlas_status st = atlas_buf_append_str(out, dir, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_append_ch(out, '/', err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_append_str(out, ATLAS_DB_FILENAME, err);
    }
    return st;
}

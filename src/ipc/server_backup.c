/* Atlas - the operator-only backup method group.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Why this file exists
 * --------------------
 * A5 made backup a local CLI operation with no RPC method, and said so in
 * `include/atlas/backup.h`: nothing reachable from MCP or a hook could replace
 * or read the index. That reasoning holds for **restore**, which replaces the
 * record, and for a single-user install, where the uid that owns the index can
 * copy the file with `cp` and Atlas is not what stands in the way.
 *
 * It does not hold for **create** and **verify** under A7.1. There the index is
 * 0700 `atlasd` and the operator is a different uid, so `atlas backup create`
 * could not open the database at all: the one account a root-owned policy names
 * as the operator was the one account that could not take a backup, and the
 * failure arrived as `there is no Atlas index to back up` — which is false, and
 * points at a file the caller was never going to see.
 *
 * So create and verify are served here, and restore is not. The asymmetry is
 * the point: reading the record out is an operator operation that A7.1 made
 * impossible by accident, while replacing the record is an operation that
 * should require stopping the daemon and standing in front of the machine.
 *
 * What this does not become
 * -------------------------
 * These are in the **operator** group, offered only to the peer whose
 * `SO_PEERCRED` uid the root-owned authority policy names — the same gate as
 * approve and reject, and the same silence for everyone else: a peer the policy
 * does not name gets `unknown method`, which is what a name that does not exist
 * gets. A model with every ordinary method still cannot reach either of these.
 *
 * The client never names a filesystem path. It may supply one path component,
 * validated against a closed character set, and the file is created inside a
 * fixed directory under the daemon's own data directory. That is what keeps
 * "the daemon writes where the client says" from ever being true: the client
 * chooses a name within one directory, not a location.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "atlas/backup.h"
#include "atlas/buf.h"
#include "atlas/error.h"
#include "atlas/ipc.h"
#include "atlas/json.h"
#include "atlas/limits.h"
#include "server_internal.h"

/* The directory backups are created in, relative to the daemon's data
 * directory. Fixed rather than configured: a configurable destination is a
 * place for a deployment to point Atlas at something it should not write to,
 * and the data directory is already the thing whose permissions the operator
 * reasons about. */
#define ATLAS_BACKUP_DIRNAME "backups"

/* The longest a client-supplied backup name may be, before the directory. */
#define ATLAS_BACKUP_NAME_MAX 96u

/* Whether `name` is one safe path component.
 *
 * Closed set, not a blocklist: letters, digits, dot, dash and underscore. That
 * refuses `/` and `\` (traversal by separator), `..` and `.` (traversal by
 * meaning), the empty string, a leading dot (hidden files), NUL and every byte
 * a shell or a path parser treats specially — without needing to enumerate what
 * those are. A name that survives this cannot leave the backup directory,
 * whatever the platform's path rules turn out to be. */
static bool safe_component(const char *name) {
    if (name == NULL) {
        return false;
    }
    size_t n = strlen(name);
    if (n == 0u || n > ATLAS_BACKUP_NAME_MAX) {
        return false;
    }
    if (name[0] == '.' || name[0] == '-') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '-' || c == '_';
        if (!ok) {
            return false;
        }
    }
    /* Belt and braces: the character set already refuses a separator, so a
     * literal `..` is the only traversal spelling left, and it is refused by
     * name. */
    if (strcmp(name, "..") == 0 || strcmp(name, ".") == 0) {
        return false;
    }
    return true;
}

/* `<data_dir>/backups`, created 0700 if it is not there yet. */
static atlas_status backup_dir(dispatch_state *ds, atlas_buf *out, atlas_err *err) {
    atlas_buf_reset(out);
    atlas_status st = atlas_buf_appendf(out, err, "%s/%s", ds->ctx->data_dir,
                                        ATLAS_BACKUP_DIRNAME);
    if (st != ATLAS_OK) {
        return st;
    }
    if (mkdir(atlas_buf_cstr(out), 0700) != 0) {
        struct stat sb;
        if (stat(atlas_buf_cstr(out), &sb) != 0 || !S_ISDIR(sb.st_mode)) {
            return atlas_err_set(err, ATLAS_ERR_CONFIG,
                                 "the Atlas backup directory could not be created");
        }
    }
    return ATLAS_OK;
}

/* Resolves a client-supplied name to an absolute path inside the backup
 * directory, refusing anything that is not one safe component. */
static atlas_status resolve_name(dispatch_state *ds, const atlas_ipc_request *req, bool required,
                                 atlas_buf *out, atlas_buf *name_out, atlas_err *err) {
    const char *name = NULL;
    (void)atlas_ipc_param_str(req, "name", &name);
    char generated[64];
    if (name == NULL || name[0] == '\0') {
        if (required) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "\"name\" names the backup to read");
        }
        /* The daemon names it, from its own clock. A client-supplied timestamp
         * would be a client describing the world, which is not evidence about
         * it. */
        (void)snprintf(generated, sizeof generated, "atlas-%lld.atlasbak",
                       (long long)time(NULL));
        name = generated;
    }
    if (!safe_component(name)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a backup name is one path component of letters, digits, '.', '-' "
                             "and '_', and may not begin with '.' or '-'");
    }
    atlas_buf dir = ATLAS_BUF_INIT;
    atlas_status st = backup_dir(ds, &dir, err);
    if (st == ATLAS_OK) {
        atlas_buf_reset(out);
        st = atlas_buf_appendf(out, err, "%s/%s", atlas_buf_cstr(&dir), name);
    }
    if (st == ATLAS_OK && name_out != NULL) {
        st = atlas_buf_set_str(name_out, name, err);
    }
    atlas_buf_free(&dir);
    return st;
}

/* The fields of a verification report, shared by both methods so a backup
 * verified inside `create` and one verified on its own read identically. */
static atlas_status write_verify(dispatch_state *ds, const atlas_backup_verify_report *rep,
                                 const char *prefix, atlas_err *err) {
    /* Flat keys with an optional prefix rather than a nested object: the IPC
     * result accessors read one level, so a nested report would need a second
     * accessor family that only this method used. `backup.create` prefixes its
     * embedded report with `verified_`; `backup.verify` uses no prefix. */
    char k[64];
#define K(field) (prefix[0] == '\0' ? (field) : (snprintf(k, sizeof k, "%s%s", prefix, field), k))
    atlas_status st = atlas_json_key_str(ds->j, K("verdict"),
                                         atlas_backup_verdict_name(rep->verdict), err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, K("ok"), rep->ok, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, K("size_bytes"), rep->size_bytes, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, K("sha256"), rep->sha256, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, K("schema_version"), rep->schema_version, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, K("expected_schema_version"), rep->expected_schema_version,
                                err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, K("revisions_checked"), rep->revisions_checked, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, K("revisions_corrupt"), rep->revisions_corrupt, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, K("ledger_mismatched"), rep->ledger_mismatched, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, K("repo_count"), rep->repo_count, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, K("integrity"), atlas_buf_cstr(&rep->integrity), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(
            ds->j, K("problems"), rep->problems.len > 0 ? atlas_buf_cstr(&rep->problems) : NULL,
            err);
    }
#undef K
    return st;
}

/* backup.create — snapshot the live index into the backup directory.
 *
 * The snapshot is taken through SQLite's online backup API from a read-only
 * connection, which is what makes it WAL-consistent without stopping the
 * writer: the daemon keeps its lock and keeps writing while this runs, and the
 * result is the database as of one commit boundary. `atlas_service_backup_create`
 * is given the daemon's own data directory rather than a caller's, and it takes
 * no lock — a backup that took the writer lock would be a backup that could
 * deadlock against the process taking it. */
static atlas_status method_backup_create(dispatch_state *ds, const atlas_ipc_request *req,
                                         atlas_err *err) {
    atlas_buf path = ATLAS_BUF_INIT;
    atlas_buf name = ATLAS_BUF_INIT;
    atlas_status st = resolve_name(ds, req, false, &path, &name, err);

    atlas_backup_report rep;
    atlas_backup_report_init(&rep);
    if (st == ATLAS_OK) {
        atlas_backup_create_opts opts;
        memset(&opts, 0, sizeof opts);
        opts.output = atlas_buf_cstr(&path);
        /* Never forced over the socket. Replacing an existing backup is a
         * destructive act on the one copy of something, and a client that can
         * pick the name could otherwise overwrite yesterday's by choosing it. */
        opts.force = false;
        st = atlas_service_backup_create(ds->ctx->data_dir, &opts, &rep, err);
    }
    /* Verified here, by the daemon, before it is reported as a backup at all.
     * A create that reported success and left an unreadable file would be worse
     * than a failure, because the operator would stop looking. */
    atlas_backup_verify_report vrep;
    atlas_backup_verify_report_init(&vrep);
    if (st == ATLAS_OK) {
        st = atlas_service_backup_verify(atlas_buf_cstr(&rep.path), &vrep, err);
    }
    if (st == ATLAS_OK && !vrep.ok) {
        st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                           "the backup was written but did not verify (%s)",
                           atlas_backup_verdict_name(vrep.verdict));
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "backup", atlas_buf_cstr(&name), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "size_bytes", rep.size_bytes, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "sha256", rep.sha256, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "atlas_version", rep.atlas_version, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "schema_version", rep.schema_version, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "source_online", rep.source_online, err);
    }
    if (st == ATLAS_OK) {
        st = write_verify(ds, &vrep, "verified_", err);
    }
    atlas_backup_verify_report_free(&vrep);
    atlas_backup_report_free(&rep);
    atlas_buf_free(&name);
    atlas_buf_free(&path);
    return st;
}

/* backup.verify — check one backup in the backup directory.
 *
 * Creates nothing and repairs nothing. A backup that is merely bad is an answer
 * rather than an error, so the method succeeds and `ok` is false; a non-OK
 * status means the question could not be asked. */
static atlas_status method_backup_verify(dispatch_state *ds, const atlas_ipc_request *req,
                                         atlas_err *err) {
    atlas_buf path = ATLAS_BUF_INIT;
    atlas_buf name = ATLAS_BUF_INIT;
    atlas_status st = resolve_name(ds, req, true, &path, &name, err);

    atlas_backup_verify_report rep;
    atlas_backup_verify_report_init(&rep);
    if (st == ATLAS_OK) {
        st = atlas_service_backup_verify(atlas_buf_cstr(&path), &rep, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "backup", atlas_buf_cstr(&name), err);
    }
    if (st == ATLAS_OK) {
        st = write_verify(ds, &rep, "", err);
    }
    atlas_backup_verify_report_free(&rep);
    atlas_buf_free(&name);
    atlas_buf_free(&path);
    return st;
}

/* Deliberately two names and not three. `backup.restore` does not exist: see
 * the file header. */
static const atlas_method_entry BACKUP_METHODS[] = {
    {"backup.create", method_backup_create},
    {"backup.verify", method_backup_verify},
};

const atlas_method_entry *atlas_server_backup_methods(size_t *count_out) {
    if (count_out != NULL) {
        *count_out = sizeof(BACKUP_METHODS) / sizeof(BACKUP_METHODS[0]);
    }
    return BACKUP_METHODS;
}

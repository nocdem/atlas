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
#include "atlas/maintenance.h"
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
    /* Emitted because the report has both and they mean different things:
     * `revisions_checked` counts the rows looked at, `revisions_rehashed`
     * counts the ones whose content was actually rehashed and compared. Only
     * the first was ever sent, and the renderers print the second — so a backup
     * verified over the socket reported "0 revision(s) rehashed" while the same
     * file verified locally reported 73. The verification had run; the number
     * proving it had run was dropped in transit, which is the worst shape this
     * kind of bug takes: it understates a check rather than failing it. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, K("revisions_rehashed"), rep->revisions_rehashed, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, K("foreign_key_check"),
                                atlas_buf_cstr(&rep->foreign_key_check), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, K("tables_required"), rep->tables_required, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, K("tables_present"), rep->tables_present, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(
            ds->j, K("missing_tables"),
            rep->missing_tables.len > 0 ? atlas_buf_cstr(&rep->missing_tables) : NULL, err);
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
    /* Validated here and nowhere else. The operations layer never validates a
     * path: a second validator is a second answer, and the two would disagree
     * the first time one of them was fixed. */
    atlas_status st = resolve_name(ds, req, false, &path, &name, err);

    int64_t op_id = 0;
    if (st == ATLAS_OK) {
        /* Never forced over the socket. Replacing an existing backup is a
         * destructive act on the one copy of something, and a client that can
         * pick the name could otherwise overwrite yesterday's by choosing it. */
        st = atlas_ops_submit_backup(ds->ctx->ops, atlas_buf_cstr(&path), false, &op_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "accepted", true, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "operation_id", op_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "operation_state",
                                atlas_op_state_name(ATLAS_OP_RUNNING), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "backup", atlas_buf_cstr(&name), err);
    }
    atlas_buf_free(&name);
    atlas_buf_free(&path);
    return st;
}

/* operation.get — the state of one accepted long operation.
 *
 * A read, and idempotent by construction: a record that reached a terminal
 * state never changes again, so asking twice cannot make the answer move. That
 * is what lets a client poll without having to reason about races, and what
 * lets a client that was killed mid-poll simply ask again.
 *
 * It reports on the operation, never on the connection that started it. The
 * work runs on a daemon thread that holds no reference to any client, so a
 * disconnected client neither cancels nor corrupts an operation in flight. */
static atlas_status method_operation_get(dispatch_state *ds, const atlas_ipc_request *req,
                                         atlas_err *err) {
    int64_t id = 0;
    if (!atlas_ipc_param_int(req, "operation_id", &id)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "operation.get needs an \"operation_id\"");
    }
    atlas_op_record rec;
    atlas_op_record_init(&rec);
    atlas_status st = atlas_ops_get(ds->ctx->ops, id, &rec, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "operation_id", rec.id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "kind", atlas_op_kind_name(rec.kind), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "state", atlas_op_state_name(rec.state), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "done", atlas_op_state_is_terminal(rec.state), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "succeeded", rec.state == ATLAS_OP_SUCCEEDED, err);
    }
    if (st == ATLAS_OK && rec.finished_at_ms > 0) {
        st = atlas_json_key_int(ds->j, "duration_ms", rec.finished_at_ms - rec.started_at_ms, err);
    }
    /* The operation's own typed result, reported rather than left for the
     * client to recompute. A client that measured the file itself would be
     * describing what it looks like now, not what the operation produced. */
    if (st == ATLAS_OK && rec.kind == ATLAS_OP_KIND_BACKUP_CREATE &&
        rec.state == ATLAS_OP_SUCCEEDED) {
        st = atlas_json_key_int(ds->j, "size_bytes", rec.size_bytes, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "page_size", rec.page_size, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "page_count", rec.page_count, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "schema_version", rec.schema_version, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_bool(ds->j, "source_online", rec.source_online, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "sha256", rec.sha256, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "atlas_version", rec.atlas_version, err);
        }
    }
    if (st == ATLAS_OK && rec.kind == ATLAS_OP_KIND_BACKUP_VERIFY &&
        rec.state == ATLAS_OP_SUCCEEDED) {
        st = write_verify(ds, &rec.verify, "", err);
    }
    /* Both are Atlas-owned text: a fixed summary this daemon wrote, or its own
     * error message. Neither can carry a repository's bytes. Encoded anyway,
     * because the rule is that everything reaching a document is encoded and an
     * exception is how the next value gets through unencoded. */
    if (st == ATLAS_OK && rec.message.len > 0) {
        st = atlas_json_key_str(ds->j, "message",
                                atlas_safe(&ds->safe, atlas_buf_cstr(&rec.message)), err);
    }
    if (st == ATLAS_OK && rec.detail.len > 0) {
        st = atlas_json_key_str(ds->j, "detail",
                                atlas_safe(&ds->safe, atlas_buf_cstr(&rec.detail)), err);
    }
    atlas_op_record_free(&rec);
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

    /* Accepted and polled, like `create`, and for the same reason: verification
     * reads every page — `PRAGMA integrity_check` walks the b-trees and every
     * decision revision is rehashed — so on a large index it takes longer than
     * a client will hold a socket open. Converting `create` and leaving this
     * one inline simply moved the timeout to the next command, which is what
     * happened: an 815 MiB backup verified fine on the daemon and the operator
     * was told "timed out while reading a frame header". */
    int64_t op_id = 0;
    if (st == ATLAS_OK) {
        st = atlas_ops_submit_verify(ds->ctx->ops, atlas_buf_cstr(&path), &op_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "accepted", true, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "operation_id", op_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "backup", atlas_buf_cstr(&name), err);
    }
    atlas_buf_free(&name);
    atlas_buf_free(&path);
    return st;
}

/* code.index — build or rebuild a semantic index for a registered repository.
 *
 * In this table, and therefore in the operator-uid group, because indexing runs
 * a compiler over repository source and writes the index. A model holding every
 * ordinary Atlas method still cannot reach it: the peer's uid must equal the
 * `operator_uid` in the root-owned policy, and every other peer is told the
 * method does not exist.
 *
 * The work is queued to the writer thread — the daemon's one serialized writer
 * path — and this returns as soon as it is accepted. Before the closeout there
 * was no method at all: an operator had to stop the service and run the index
 * as the service account, which is exactly the undocumented workaround the
 * closeout forbids. `service.h` had already described this method as existing,
 * which made the documentation wrong as well as the product incomplete. */
static atlas_status method_code_index(dispatch_state *ds, const atlas_ipc_request *req,
                                      atlas_err *err) {
    const char *name = NULL;
    if (!atlas_ipc_param_str(req, "repo", &name) || name == NULL || name[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "code.index needs a \"repo\"");
    }
    /* Resolved from the registry here, so an unregistered name is refused
     * before any work is queued and the caller gets NOT_REGISTERED rather than
     * an operation that fails later for a reason it has to poll to learn. */
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_server_require_repo(ds, req, &info, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }

    /* A NUL-separated list of repository-relative paths. Never discovered: the
     * caller names them or there is nothing to do, which is A8-CI's rule that
     * Atlas does not search a repository for a file telling it how to compile
     * things. */
    atlas_buf list = ATLAS_BUF_INIT;
    const atlas_ipc_array *arr = NULL;
    if (atlas_ipc_param_array(req, "compdbs", &arr)) {
        size_t count = atlas_ipc_array_len(arr);
        for (size_t i = 0; i < count && st == ATLAS_OK; i++) {
            const char *one = NULL;
            if (atlas_ipc_array_str(arr, i, &one) && one != NULL && one[0] != '\0') {
                st = atlas_buf_append(&list, one, strlen(one) + 1u, err);
            }
        }
    }
    if (st == ATLAS_OK && list.len == 0) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE,
                           "name at least one compilation database with --compdb; Atlas does not "
                           "search a repository for one");
    }

    bool rebuild = false;
    (void)atlas_ipc_param_bool(req, "rebuild", &rebuild);

    int64_t op_id = 0;
    if (st == ATLAS_OK) {
        /* Refused deterministically when one is already in flight for this
         * repository, naming it — two indexes of one repository differ only in
         * which generation an operator ends up looking at. */
        st = atlas_ops_begin_sem_index(ds->ctx->ops, info.id, &op_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_writer_submit_sem_index(ds->ctx->writer, name, (const char *)list.data,
                                           list.len, rebuild, op_id, err);
        if (st != ATLAS_OK) {
            /* The record exists and nothing will ever run for it, so it is
             * closed here rather than left RUNNING for ever. */
            atlas_ops_finish(ds->ctx->ops, op_id, st, atlas_err_msg(err), "");
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "accepted", true, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "operation_id", op_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "operation_state", atlas_op_state_name(ATLAS_OP_RUNNING),
                                err);
    }
    atlas_buf_free(&list);
    atlas_repo_info_free(&info);
    return st;
}

/* Writes one maintenance report. Shared by both methods so a plan and an
 * applied prune read identically. */
static atlas_status write_maintenance(dispatch_state *ds, const atlas_maintenance_report *rep,
                                      atlas_err *err) {
    atlas_status st = atlas_json_key_bool(ds->j, "applied", rep->applied, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "older_than_days", rep->older_than_days, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "retain_per_repo", rep->retain_per_repo, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "cutoff", rep->cutoff, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "total_rows", rep->total_rows, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "total_eligible", rep->total_eligible, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "total_removed", rep->total_removed, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "prunable_tables", (int64_t)rep->prunable_tables, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "protected_tables", (int64_t)rep->protected_tables, err);
    }
    /* Every table, with its class and the written reason. The reason is the
     * deliverable: a classification without one is a label, and a label is what
     * lets a later phase quietly reclassify a table. All of it is Atlas-owned
     * text — no table name or reason comes from a repository. */
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "tables", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    for (size_t i = 0; st == ATLAS_OK && i < rep->table_count; i++) {
        const atlas_maintenance_row *row = &rep->tables[i];
        st = atlas_json_obj_begin(ds->j, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "table", row->table, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "class", atlas_retention_class_name(row->cls), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_bool(ds->j, "prunable", row->prunable, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "reason", row->reason, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_bool(ds->j, "counted", row->counted, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "rows_before", row->rows_before, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "rows_eligible", row->rows_eligible, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "rows_removed", row->rows_removed, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "rows_after", row->rows_after, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_obj_end(ds->j, err);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    return st;
}

static void take_maintenance_opts(const atlas_ipc_request *req, atlas_maintenance_opts *o) {
    memset(o, 0, sizeof(*o));
    int64_t n = 0;
    if (atlas_ipc_param_int(req, "older_than_days", &n)) {
        o->older_than_days = n;
    }
    if (atlas_ipc_param_int(req, "retain_per_repo", &n)) {
        o->retain_per_repo = n;
    }
}

/* maintenance.plan — what a prune would remove. A read: it opens nothing
 * writable, takes no lock and writes no byte.
 *
 * In the operator-uid group, not the ordinary one. A5 gave maintenance no RPC
 * surface at all, on the reasoning that the account owning the data directory
 * could do it locally anyway — and A7.1 broke that premise without anyone
 * noticing, exactly as it did for `backup.create`: under a system deployment
 * the index is 0700 `atlasd`, so the operator account could not plan or prune
 * at all and had to become the service account. The guarantee A5 actually
 * wanted is that *nothing a model can reach* may prune the index, and that is
 * unchanged: this group is gated on SO_PEERCRED against the root-owned policy,
 * and every other peer is told the method does not exist. */
static atlas_status method_maintenance_plan(dispatch_state *ds, const atlas_ipc_request *req,
                                            atlas_err *err) {
    atlas_maintenance_opts o;
    take_maintenance_opts(req, &o);
    o.apply = false;
    atlas_maintenance_report rep;
    atlas_maintenance_report_init(&rep);
    atlas_status st = atlas_maintenance_on(ds->db, &o, &rep, err);
    if (st == ATLAS_OK) {
        st = write_maintenance(ds, &rep, err);
    }
    atlas_maintenance_report_free(&rep);
    return st;
}

/* maintenance.prune — the one bounded delete, on the writer thread.
 *
 * It writes, so it happens where every write in the daemon happens. The delete
 * is per batch rather than per loop, which is A1's rule about never holding a
 * write transaction across unbounded work, and that is unchanged by running it
 * here. `--apply` is still required and still checked by the caller: a prune
 * that could be triggered without it would make the plan/apply split
 * decorative. */
static atlas_status method_maintenance_prune(dispatch_state *ds, const atlas_ipc_request *req,
                                             atlas_err *err) {
    bool apply = false;
    (void)atlas_ipc_param_bool(req, "apply", &apply);
    if (!apply) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "refusing to prune without --apply; `atlas maintenance plan` reports "
                             "what would be removed and writes nothing");
    }
    atlas_maintenance_opts o;
    take_maintenance_opts(req, &o);
    o.apply = true;
    atlas_maintenance_report rep;
    atlas_maintenance_report_init(&rep);
    atlas_status st = atlas_writer_maintenance(ds->ctx->writer, &o, &rep, err);
    if (st == ATLAS_OK) {
        st = write_maintenance(ds, &rep, err);
    }
    atlas_maintenance_report_free(&rep);
    return st;
}

/* Deliberately two names and not three. `backup.restore` does not exist: see
 * the file header. */
static const atlas_method_entry BACKUP_METHODS[] = {
    {"backup.create", method_backup_create},
    {"operation.get", method_operation_get},
    {"code.index", method_code_index},
    {"maintenance.plan", method_maintenance_plan},
    {"maintenance.prune", method_maintenance_prune},
    {"backup.verify", method_backup_verify},
};

const atlas_method_entry *atlas_server_backup_methods(size_t *count_out) {
    if (count_out != NULL) {
        *count_out = sizeof(BACKUP_METHODS) / sizeof(BACKUP_METHODS[0]);
    }
    return BACKUP_METHODS;
}

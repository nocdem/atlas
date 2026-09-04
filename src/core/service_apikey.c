/* Atlas - A9: creating, listing, revoking and rotating remote credentials.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Credential administration is an **operator** function, and A9 keeps it that
 * way structurally rather than by checking a flag. There is no MCP tool, no
 * ordinary RPC method and no gateway route that reaches anything in this file.
 * The gateway process runs as its own account, which is neither the operator uid
 * nor a dispatcher uid, so the operator-gated methods that do reach these
 * functions answer `unknown method` to it — the same answer a name that does not
 * exist gets.
 *
 * The one-time secret is the property this file exists to hold up. A create
 * returns the plaintext in an `atlas_apikey_created`, the caller prints it once,
 * and `atlas_apikey_created_free` wipes it. Nothing else can produce it: the
 * verifier is one-way, no column holds it, no read returns it, and no audit row
 * or log line carries it. "Shown once" is therefore not a promise about
 * discipline — after that free there is no copy anywhere in the process or on
 * disk.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/datadir.h"
#include "atlas/gw.h"
#include "atlas/ipc.h"
#include "atlas/json.h"
#include "atlas/lock.h"

void atlas_apikey_created_free(atlas_apikey_created *c) {
    if (c == NULL) {
        return;
    }
    /* Through a volatile pointer so the compiler may not treat the stores as
     * dead. This is the only copy of a live credential. */
    volatile unsigned char *p = (volatile unsigned char *)c;
    for (size_t i = 0; i < sizeof(*c); i++) {
        p[i] = 0;
    }
}

void atlas_apikey_listing_init(atlas_apikey_listing *l) {
    memset(l, 0, sizeof(*l));
}

void atlas_apikey_listing_free(atlas_apikey_listing *l) {
    if (l == NULL) {
        return;
    }
    free(l->keys);
    l->keys = NULL;
    l->count = 0;
}

bool atlas_apikey_id_normalise(const char *given, char *out) {
    out[0] = '\0';
    if (given == NULL) {
        return false;
    }
    const char *p = given;
    const size_t plen = sizeof(ATLAS_APIKEY_ID_PREFIX) - 1u;
    if (strncmp(p, ATLAS_APIKEY_ID_PREFIX, plen) == 0) {
        p += plen;
    }
    size_t n = strlen(p);
    if (n != ATLAS_APIKEY_SELECTOR_HEX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        /* Lowercase only. One credential has one spelling, for the reason the
         * token parser refuses uppercase hex. */
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

/* --- the core operations --------------------------------------------------- */

atlas_status atlas_apikey_create_on(atlas_db *db, const atlas_apikey_create_opts *opts,
                                    atlas_apikey_created *out, atlas_err *err) {
    memset(out, 0, sizeof(*out));

    if (!atlas_apikey_label_valid(opts->label)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a label must be 1 to %u printable characters, without a quote, "
                             "backslash or percent",
                             (unsigned)ATLAS_APIKEY_LABEL_MAX);
    }
    if (opts->scopes != 0u && opts->no_scopes) {
        /* The CLI refuses this combination itself (a better message, because
         * it still has the raw `--scope`/`--no-scopes` argv to point at), but
         * this is the write point both the local and the operator-gated
         * socket path reach, and the socket path builds `opts` from request
         * parameters nothing stops a raw caller from sending together. No
         * authority would move if this fell through silently — `no_scopes`
         * would simply be ignored and the key would hold exactly the scopes
         * asked for — but a flag a caller can set and have ignored without
         * being told is a missing refusal, not a harmless one. */
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "--scope and --no-scopes cannot both be given");
    }
    if (opts->scopes == 0u && !opts->no_scopes) {
        /* A credential with no scopes authorises nothing. Storing one is
         * coherent; creating one by accident is not, so it takes an explicit
         * scope to make a key at all — or the deliberate `--no-scopes` form,
         * A16's remote-disposal credential (Decision 2), which is the one
         * place this rule is relaxed and only by a flag whose name says so. */
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "at least one --scope is required, or --no-scopes for a "
                             "remote-disposal credential; a credential with no scopes could "
                             "not read anything");
    }
    /* Every requested scope must be one an operator may grant. `memory:write`
     * is in the vocabulary and is not grantable, which is what makes "no A9
     * credential can write" one refusal rather than a rule every tool has to
     * remember. `decisions:dispose` is refused by its own name: it is never
     * stored on a key an operator creates, only derived by the daemon for the
     * one credential a root-owned policy line names (Decision 2), so the
     * refusal says how such a credential is actually made rather than only
     * that this attempt failed. */
    for (int s = 1; s < (int)ATLAS_SCOPE__COUNT; s++) {
        atlas_apikey_scope sc = (atlas_apikey_scope)s;
        if (!atlas_scope_has(opts->scopes, sc)) {
            continue;
        }
        if (sc == ATLAS_SCOPE_DECISIONS_DISPOSE) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "decisions:dispose cannot be granted to a credential; it is "
                                 "derived for the key /etc/atlas/gateway.conf names, and only "
                                 "for one that holds no stored scope");
        }
        if (!atlas_apikey_scope_grantable(sc)) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "the scope \"%s\" cannot be granted to a remote credential in A9",
                                 atlas_apikey_scope_name(sc));
        }
    }

    char rotate_from[ATLAS_APIKEY_SELECTOR_HEX + 1];
    rotate_from[0] = '\0';
    if (opts->rotate_from != NULL && opts->rotate_from[0] != '\0') {
        if (!atlas_apikey_id_normalise(opts->rotate_from, rotate_from)) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "a key id is %u lowercase hex characters, optionally written "
                                 "\"" ATLAS_APIKEY_ID_PREFIX "<id>\"",
                                 (unsigned)ATLAS_APIKEY_SELECTOR_HEX);
        }
    }

    int64_t existing = 0;
    atlas_status st = atlas_db_apikey_count(db, &existing, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (existing >= (int64_t)ATLAS_APIKEY_MAX_KEYS) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "this index already holds %u credentials, which is the ceiling; "
                             "revoke one before creating another",
                             (unsigned)ATLAS_APIKEY_MAX_KEYS);
    }

    atlas_buf scopes = ATLAS_BUF_INIT;
    st = atlas_apikey_scopes_render(opts->scopes, &scopes, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&scopes);
        return st;
    }
    if (scopes.len >= ATLAS_APIKEY_SCOPES_MAX) {
        atlas_buf_free(&scopes);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "the scope list does not fit");
    }

    atlas_apikey_material m;
    st = atlas_apikey_generate(&m, err);
    if (st != ATLAS_OK) {
        /* Randomness failed. Nothing is written and nothing partial exists:
         * Atlas does not issue a credential it could not make unpredictable. */
        atlas_apikey_material_free(&m);
        atlas_buf_free(&scopes);
        return st;
    }

    atlas_apikey_record rec;
    memset(&rec, 0, sizeof(rec));
    (void)snprintf(rec.key_id, sizeof rec.key_id, "%s", m.key_id);
    (void)snprintf(rec.label, sizeof rec.label, "%s", opts->label);
    (void)snprintf(rec.scopes, sizeof rec.scopes, "%s", atlas_buf_cstr(&scopes));
    memcpy(rec.salt, m.salt, sizeof rec.salt);
    memcpy(rec.verifier, m.verifier, sizeof rec.verifier);
    rec.status = ATLAS_APIKEY_STATUS_ACTIVE;
    atlas_now_iso8601(rec.created_at, sizeof rec.created_at);
    (void)snprintf(rec.rotated_from, sizeof rec.rotated_from, "%s", rotate_from);

    /* One transaction. A rotation that inserted the new key and then failed to
     * revoke the old one would leave two live credentials where an operator
     * asked for one, and nothing about the result would say so. */
    st = atlas_db_begin(db, err);
    if (st == ATLAS_OK) {
        st = atlas_db_apikey_insert(db, &rec, err);
    }
    bool previous_revoked = false;
    if (st == ATLAS_OK && rotate_from[0] != '\0') {
        st = atlas_db_apikey_revoke(db, rotate_from, &previous_revoked, err);
        if (st == ATLAS_OK && !previous_revoked) {
            /* The key named either does not exist or was already revoked.
             * Reported through the result rather than failed: the new
             * credential is valid either way, and pretending the rotation
             * revoked something it did not would be the lie. */
        }
        if (st == ATLAS_OK) {
            st = atlas_db_apikey_link_rotation(db, rotate_from, rec.key_id, err);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_db_commit(db, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_rollback(db);
        atlas_apikey_material_free(&m);
        atlas_buf_free(&scopes);
        return st;
    }

    (void)snprintf(out->key_id, sizeof out->key_id, "%s", m.key_id);
    (void)snprintf(out->token, sizeof out->token, "%s", m.token);
    (void)snprintf(out->label, sizeof out->label, "%s", rec.label);
    (void)snprintf(out->scopes, sizeof out->scopes, "%s", rec.scopes);
    (void)snprintf(out->created_at, sizeof out->created_at, "%s", rec.created_at);
    (void)snprintf(out->rotated_from, sizeof out->rotated_from, "%s", rotate_from);
    out->previous_revoked = previous_revoked;

    /* The material's own copy goes now. From here the plaintext exists in
     * exactly one place — `out->token` — and the caller wipes that. */
    atlas_apikey_material_free(&m);
    atlas_buf_free(&scopes);
    return ATLAS_OK;
}

atlas_status atlas_apikey_revoke_on(atlas_db *db, const char *key_id, bool *changed,
                                    atlas_err *err) {
    *changed = false;
    char id[ATLAS_APIKEY_SELECTOR_HEX + 1];
    if (!atlas_apikey_id_normalise(key_id, id)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a key id is %u lowercase hex characters, optionally written "
                             "\"" ATLAS_APIKEY_ID_PREFIX "<id>\"",
                             (unsigned)ATLAS_APIKEY_SELECTOR_HEX);
    }
    /* Revocation takes effect the moment this commits. There is no cached
     * verdict anywhere: the gateway asks the daemon on every request, so a
     * revoked credential stops working immediately rather than at the end of
     * some session. That is why there is no session cache to invalidate. */
    atlas_status st = atlas_db_begin(db, err);
    if (st == ATLAS_OK) {
        st = atlas_db_apikey_revoke(db, id, changed, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_commit(db, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_rollback(db);
    }
    return st;
}

typedef struct collect {
    atlas_apikey_listing *out;
    size_t cap;
} collect;

static atlas_status collect_row(const atlas_apikey_record *rec, void *ud, atlas_err *err) {
    collect *c = (collect *)ud;
    if (c->out->count == c->cap) {
        size_t next = c->cap == 0 ? 16u : c->cap * 2u;
        if (next > ATLAS_APIKEY_MAX_KEYS) {
            next = ATLAS_APIKEY_MAX_KEYS;
        }
        if (next == c->cap) {
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "too many credentials to list");
        }
        atlas_apikey_record *grown = realloc(c->out->keys, next * sizeof(*grown));
        if (grown == NULL) {
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory listing credentials");
        }
        c->out->keys = grown;
        c->cap = next;
    }
    /* Copied, not aliased: the callback's pointer is borrowed and this record
     * outlives the statement. */
    c->out->keys[c->out->count++] = *rec;
    return ATLAS_OK;
}

atlas_status atlas_apikey_list_on(atlas_db *db, atlas_apikey_listing *out, atlas_err *err) {
    atlas_apikey_listing_free(out);
    collect c = {out, 0};
    atlas_status st = atlas_db_apikey_list(db, collect_row, &c, NULL, err);
    if (st != ATLAS_OK) {
        atlas_apikey_listing_free(out);
    }
    return st;
}

/* --- local entry points ---------------------------------------------------- */

/* Opens the index and, for a write, takes the data-directory writer lock
 * exclusively. Atlas has exactly one writer, and taking the lock is what makes
 * "the daemon must be stopped" a fact the kernel enforces rather than a line in
 * a manual — A5's rule for restore and prune, applied here for the same reason.
 *
 * `writable` selects the lock and the handle together, because a read that took
 * the lock would block the daemon for no reason and a write that did not would
 * be a second writer. */
static atlas_status open_local(const char *data_dir_override, bool writable, atlas_buf *data_dir,
                               atlas_lock **lk, atlas_db **db, atlas_err *err) {
    atlas_buf db_path = ATLAS_BUF_INIT;
    *lk = NULL;
    *db = NULL;
    atlas_status st = atlas_datadir_resolve(data_dir_override, data_dir, NULL, err);
    if (st == ATLAS_OK) {
        st = atlas_datadir_db_path(atlas_buf_cstr(data_dir), &db_path, err);
    }
    if (st == ATLAS_OK && writable) {
        st = atlas_datadir_ensure(atlas_buf_cstr(data_dir), err);
    }
    if (st == ATLAS_OK && writable) {
        st = atlas_lock_acquire(atlas_buf_cstr(data_dir), ATLAS_LOCK_ROLE_ONESHOT, lk, err);
    }
    if (st == ATLAS_OK) {
        st = writable ? atlas_db_open(atlas_buf_cstr(&db_path), db, err)
                      : atlas_db_open_readonly(atlas_buf_cstr(&db_path), db, err);
    }
    /* Opening does not migrate; that is a separate, deliberate call. The
     * writable path takes it because creating the first credential on a machine
     * where Atlas has never run must work — that is the bootstrap this command
     * exists for. The read-only path cannot migrate and must not try: a listing
     * against an index from an older Atlas reports what it can read rather than
     * upgrading a database nobody asked it to upgrade. */
    if (st == ATLAS_OK && writable) {
        st = atlas_db_migrate(*db, err);
    }
    atlas_buf_free(&db_path);
    return st;
}

static void close_local(atlas_buf *data_dir, atlas_lock *lk, atlas_db *db) {
    atlas_db_close(db);
    atlas_lock_release(lk);
    atlas_buf_free(data_dir);
}

atlas_status atlas_service_apikey_create(const char *data_dir_override,
                                         const atlas_apikey_create_opts *opts,
                                         atlas_apikey_created *out, atlas_err *err) {
    atlas_buf data_dir = ATLAS_BUF_INIT;
    atlas_lock *lk = NULL;
    atlas_db *db = NULL;
    atlas_status st = open_local(data_dir_override, true, &data_dir, &lk, &db, err);
    if (st == ATLAS_OK) {
        st = atlas_apikey_create_on(db, opts, out, err);
    }
    close_local(&data_dir, lk, db);
    return st;
}

atlas_status atlas_service_apikey_revoke(const char *data_dir_override, const char *key_id,
                                         bool *changed, atlas_err *err) {
    atlas_buf data_dir = ATLAS_BUF_INIT;
    atlas_lock *lk = NULL;
    atlas_db *db = NULL;
    atlas_status st = open_local(data_dir_override, true, &data_dir, &lk, &db, err);
    if (st == ATLAS_OK) {
        st = atlas_apikey_revoke_on(db, key_id, changed, err);
    }
    close_local(&data_dir, lk, db);
    return st;
}

atlas_status atlas_service_apikey_list(const char *data_dir_override, atlas_apikey_listing *out,
                                       atlas_err *err) {
    atlas_buf data_dir = ATLAS_BUF_INIT;
    atlas_lock *lk = NULL;
    atlas_db *db = NULL;
    /* A listing is a read: no lock, a read-only handle, and it can run while
     * the daemon is serving. */
    atlas_status st = open_local(data_dir_override, false, &data_dir, &lk, &db, err);
    if (st == ATLAS_OK) {
        st = atlas_apikey_list_on(db, out, err);
    }
    close_local(&data_dir, lk, db);
    return st;
}

/* --- the socket path ------------------------------------------------------
 *
 * When a daemon owns this data directory it holds the writer lock, so the local
 * path above cannot take it — and under A7.1 the operator cannot open the index
 * at all. Both cases are answered by asking the daemon, over the operator-gated
 * methods in `src/ipc/server_apikey.c`.
 *
 * The CLI chooses between the two exactly as `route_to_daemon()` does for every
 * other write: the daemon must own *this* directory, or the command runs
 * locally and takes that directory's own lock. */

static atlas_status call_daemon(const char *method, const char *params, atlas_ipc_response **out,
                                atlas_err *err) {
    *out = NULL;
    atlas_buf sock = ATLAS_BUF_INIT;
    atlas_status st = atlas_ipc_socket_path(&sock, err);
    atlas_buf resp = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_ipc_call(atlas_buf_cstr(&sock), method, params, &resp, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_ipc_response_parse(resp.data, resp.len, out, err);
    }
    if (st == ATLAS_OK && !atlas_ipc_response_ok(*out)) {
        st = atlas_err_set(err, atlas_ipc_response_status(*out), "%s",
                           atlas_ipc_response_message(*out));
        atlas_ipc_response_free(*out);
        *out = NULL;
    }
    atlas_buf_free(&resp);
    atlas_buf_free(&sock);
    return st;
}

atlas_status atlas_service_apikey_create_remote(const atlas_apikey_create_opts *opts,
                                                atlas_apikey_created *out, atlas_err *err) {
    memset(out, 0, sizeof(*out));
    atlas_buf scopes = ATLAS_BUF_INIT;
    atlas_status st = atlas_apikey_scopes_render(opts->scopes, &scopes, err);
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        atlas_ipc_params *p = NULL;
        atlas_json *j = NULL;
        st = atlas_ipc_params_begin(&p, &j, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "label", opts->label != NULL ? opts->label : "", err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "scopes", atlas_buf_cstr(&scopes), err);
        }
        if (st == ATLAS_OK && opts->no_scopes) {
            /* Forwarded explicitly: the daemon must see the same deliberate
             * flag the local write point does, never infer it from
             * `scopes` being empty (Decision 2's "never a silent
             * relaxation"). Omitted rather than sent `false` when unset, so
             * an older daemon that does not know this key still sees exactly
             * what it always saw. */
            st = atlas_json_key_bool(j, "no_scopes", true, err);
        }
        if (st == ATLAS_OK && opts->rotate_from != NULL && opts->rotate_from[0] != '\0') {
            st = atlas_json_key_str(j, "rotate_from", opts->rotate_from, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_ipc_params_finish(p, &params, err);
        } else {
            atlas_ipc_params_abort(p);
        }
    }
    atlas_ipc_response *r = NULL;
    if (st == ATLAS_OK) {
        st = call_daemon("apikey.create", atlas_buf_cstr(&params), &r, err);
    }
    if (st == ATLAS_OK) {
        const char *v = NULL;
        if (atlas_ipc_result_obj_str(r, "api_key", "id", &v) && v != NULL) {
            (void)snprintf(out->key_id, sizeof out->key_id, "%s", v);
        }
        if (atlas_ipc_result_obj_str(r, "api_key", "label", &v) && v != NULL) {
            (void)snprintf(out->label, sizeof out->label, "%s", v);
        }
        if (atlas_ipc_result_obj_str(r, "api_key", "scopes", &v) && v != NULL) {
            (void)snprintf(out->scopes, sizeof out->scopes, "%s", v);
        }
        if (atlas_ipc_result_obj_str(r, "api_key", "created_at", &v) && v != NULL) {
            (void)snprintf(out->created_at, sizeof out->created_at, "%s", v);
        }
        if (atlas_ipc_result_obj_str(r, "api_key", "rotated_from", &v) && v != NULL) {
            (void)snprintf(out->rotated_from, sizeof out->rotated_from, "%s", v);
        }
        bool b = false;
        if (atlas_ipc_result_obj_bool(r, "api_key", "previous_revoked", &b)) {
            out->previous_revoked = b;
        }
        if (atlas_ipc_result_obj_str(r, "api_key", "secret", &v) && v != NULL) {
            (void)snprintf(out->token, sizeof out->token, "%s", v);
        }
        if (out->token[0] == '\0') {
            /* A create that produced no plaintext is a create the operator
             * cannot use, and reporting success would leave them with a
             * credential nobody holds. */
            st = atlas_err_set(err, ATLAS_ERR_INTERNAL,
                               "the daemon created a credential but returned no secret");
        }
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&params);
    atlas_buf_free(&scopes);
    if (st != ATLAS_OK) {
        atlas_apikey_created_free(out);
    }
    return st;
}

atlas_status atlas_service_apikey_revoke_remote(const char *key_id, bool *changed,
                                                atlas_err *err) {
    *changed = false;
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_ipc_params *p = NULL;
    atlas_json *j = NULL;
    atlas_status st = atlas_ipc_params_begin(&p, &j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "key_id", key_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_ipc_params_finish(p, &params, err);
    } else {
        atlas_ipc_params_abort(p);
    }
    atlas_ipc_response *r = NULL;
    if (st == ATLAS_OK) {
        st = call_daemon("apikey.revoke", atlas_buf_cstr(&params), &r, err);
    }
    if (st == ATLAS_OK) {
        bool b = false;
        if (atlas_ipc_result_bool(r, "changed", &b)) {
            *changed = b;
        }
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&params);
    return st;
}

typedef struct listing_build {
    atlas_apikey_listing *out;
    size_t cap;
} listing_build;

atlas_status atlas_service_apikey_list_remote(atlas_apikey_listing *out, atlas_err *err) {
    atlas_apikey_listing_free(out);
    atlas_ipc_response *r = NULL;
    atlas_status st = call_daemon("apikey.list", "{}", &r, err);
    if (st != ATLAS_OK) {
        return st;
    }
    size_t n = 0;
    (void)atlas_ipc_result_arr_len(r, "api_keys", &n);
    if (n > 0) {
        out->keys = calloc(n, sizeof(*out->keys));
        if (out->keys == NULL) {
            atlas_ipc_response_free(r);
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory listing credentials");
        }
    }
    for (size_t i = 0; i < n; i++) {
        atlas_apikey_record *k = &out->keys[out->count];
        memset(k, 0, sizeof(*k));
        const char *v = NULL;
        if (atlas_ipc_result_arr_obj_str(r, "api_keys", i, "id", &v) && v != NULL) {
            (void)snprintf(k->key_id, sizeof k->key_id, "%s", v);
        }
        if (atlas_ipc_result_arr_obj_str(r, "api_keys", i, "label", &v) && v != NULL) {
            (void)snprintf(k->label, sizeof k->label, "%s", v);
        }
        if (atlas_ipc_result_arr_obj_str(r, "api_keys", i, "scopes", &v) && v != NULL) {
            (void)snprintf(k->scopes, sizeof k->scopes, "%s", v);
        }
        if (atlas_ipc_result_arr_obj_str(r, "api_keys", i, "status", &v) && v != NULL) {
            k->status = atlas_apikey_status_parse(v);
        }
        if (atlas_ipc_result_arr_obj_str(r, "api_keys", i, "created_at", &v) && v != NULL) {
            (void)snprintf(k->created_at, sizeof k->created_at, "%s", v);
        }
        if (atlas_ipc_result_arr_obj_str(r, "api_keys", i, "revoked_at", &v) && v != NULL) {
            (void)snprintf(k->revoked_at, sizeof k->revoked_at, "%s", v);
        }
        if (atlas_ipc_result_arr_obj_str(r, "api_keys", i, "last_used_at", &v) && v != NULL) {
            (void)snprintf(k->last_used_at, sizeof k->last_used_at, "%s", v);
        }
        if (atlas_ipc_result_arr_obj_str(r, "api_keys", i, "rotated_from", &v) && v != NULL) {
            (void)snprintf(k->rotated_from, sizeof k->rotated_from, "%s", v);
        }
        if (atlas_ipc_result_arr_obj_str(r, "api_keys", i, "rotated_to", &v) && v != NULL) {
            (void)snprintf(k->rotated_to, sizeof k->rotated_to, "%s", v);
        }
        bool b = false;
        if (atlas_ipc_result_arr_obj_bool(r, "api_keys", i, "scopes_unreadable", &b)) {
            k->scopes_unreadable = b;
        }
        atlas_err serr;
        atlas_err_init(&serr);
        if (atlas_apikey_scopes_parse(k->scopes, &k->mask, &serr) != ATLAS_OK) {
            k->mask = 0u;
            k->scopes_unreadable = true;
        }
        out->count++;
    }
    atlas_ipc_response_free(r);
    return ATLAS_OK;
}

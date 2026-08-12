/* Atlas - A9 storage: remote credentials and the gateway audit trail.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Typed operations over the migration-12 tables. `sqlite3` types never leave
 * `src/db`, which is the rule every other storage layer in Atlas follows.
 *
 * Two records that answer two different questions, and they are kept apart on
 * purpose. `api_keys` is **who may ask**. `gw_audit` is **what was asked**. An
 * audit row therefore holds the key id as plain text and not as a foreign key:
 * the account of what a credential did has to survive the credential, and a
 * revoked key whose history vanished with it is exactly the case an operator
 * most needs to read.
 *
 * Every field here is fixed-size. That is not an optimisation — it means a
 * record can be returned by value from a row callback whose borrowed pointers
 * die with the statement, which is the ownership mistake this codebase's row
 * callbacks are documented to invite.
 *
 * **No structure here can hold a plaintext secret.** There is no field for one,
 * so "the secret is never stored" is a property of the type rather than a rule
 * somebody has to keep.
 */
#ifndef ATLAS_GW_H
#define ATLAS_GW_H

#include <stdbool.h>
#include <stdint.h>

#include "atlas/apikey.h"
#include "atlas/db.h"
#include "atlas/error.h"
#include "atlas/limits.h"

/* Every scope name plus separators, with room for the vocabulary to grow. A
 * stored list longer than this cannot be one Atlas wrote. */
#define ATLAS_APIKEY_SCOPES_MAX 256u
/* The `operation` and `detail` columns of an audit row. `detail` is fixed Atlas
 * text or a safe-encoded failure message, never a request body. */
#define ATLAS_GW_AUDIT_OP_MAX 96u
#define ATLAS_GW_AUDIT_DETAIL_MAX 512u
/* Rows one audit listing returns. Bounded like every other Atlas read; reaching
 * it is reported, never a silent truncation. */
#define ATLAS_GW_AUDIT_MAX_ROWS 500

/* How often an authenticated request may refresh `last_used_at`.
 *
 * A timestamp written on every call would put a database write on the path of
 * every remote read, which would make the daemon's single writer the gateway's
 * bottleneck and turn an unauthenticated flood into write amplification. This
 * column is evidence that a credential is in use, not an access log — `gw_audit`
 * is the access log, and it records every request. */
#define ATLAS_APIKEY_TOUCH_INTERVAL_MS (5 * 60 * 1000)

/* --- the credential record ------------------------------------------------ */

typedef struct atlas_apikey_record {
    int64_t id; /* the rowid; never leaves Atlas and never identifies a key */
    char key_id[ATLAS_APIKEY_SELECTOR_HEX + 1];
    char label[ATLAS_APIKEY_LABEL_MAX + 1];
    char scopes[ATLAS_APIKEY_SCOPES_MAX];
    /* Parsed from `scopes` on read. Zero when the stored list held a name this
     * binary does not know, which is a refusal rather than a smaller grant —
     * see `scopes_unreadable`. */
    atlas_scope_mask mask;
    /* True when `scopes` could not be parsed. The key is then unusable, and the
     * distinction matters: "this credential grants nothing" and "this Atlas
     * cannot tell what this credential grants" call for different actions. */
    bool scopes_unreadable;
    unsigned char salt[ATLAS_APIKEY_SALT_BYTES];
    unsigned char verifier[ATLAS_APIKEY_VERIFIER_BYTES];
    atlas_apikey_status status;
    char created_at[ATLAS_TS_MAX];
    char revoked_at[ATLAS_TS_MAX];
    char last_used_at[ATLAS_TS_MAX];
    char rotated_from[ATLAS_APIKEY_SELECTOR_HEX + 1];
    char rotated_to[ATLAS_APIKEY_SELECTOR_HEX + 1];
} atlas_apikey_record;

/* Inserts one credential.
 *
 * The caller has already generated the material and validated the label and the
 * scopes; this writes what it is given and refuses a duplicate `key_id` rather
 * than replacing one. There is no upsert, deliberately: a credential is created
 * once, and an "insert or replace" would let a second create silently take over
 * an existing key id. */
atlas_status atlas_db_apikey_insert(atlas_db *db, const atlas_apikey_record *rec, atlas_err *err);

/* Looks one credential up by its selector.
 *
 * `*found` is false when there is no such key, and that is not an error: an
 * unknown selector is the ordinary outcome of somebody presenting a token Atlas
 * never issued, and it must cost the same as a wrong secret. */
atlas_status atlas_db_apikey_lookup(atlas_db *db, const char *key_id, atlas_apikey_record *out,
                                    bool *found, atlas_err *err);

/* Every credential, newest first. Bounded by ATLAS_APIKEY_MAX_KEYS.
 *
 * The callback receives a record by value, so nothing it keeps points into a
 * live statement. */
typedef atlas_status (*atlas_apikey_row_cb)(const atlas_apikey_record *rec, void *ud,
                                            atlas_err *err);
atlas_status atlas_db_apikey_list(atlas_db *db, atlas_apikey_row_cb cb, void *ud, int64_t *count_out,
                                  atlas_err *err);

atlas_status atlas_db_apikey_count(atlas_db *db, int64_t *out, atlas_err *err);

/* Revokes one credential.
 *
 * The UPDATE names the state it observed — `WHERE key_id = ? AND status =
 * 'ACTIVE'` — and the caller requires exactly one changed row, which is A4's
 * rule about compare-and-swap. `*changed` is false when the key does not exist
 * or was already revoked; revoking twice is not an error, because the outcome
 * an operator asked for already holds. */
atlas_status atlas_db_apikey_revoke(atlas_db *db, const char *key_id, bool *changed,
                                    atlas_err *err);

/* Records that a credential was used, at most once per
 * ATLAS_APIKEY_TOUCH_INTERVAL_MS.
 *
 * Cheap and idempotent, and it deliberately does not fail a request: a
 * credential that authenticated must not be refused because a timestamp could
 * not be written. The caller reports the failure and continues. */
atlas_status atlas_db_apikey_touch(atlas_db *db, const char *key_id, atlas_err *err);

/* Records a rotation as a link in both directions.
 *
 * Rotation is create-then-revoke: the new key exists before the old one stops
 * working, so an operator can install it without a gap. Recording the link from
 * both ends is what lets `atlas api-key list` explain why a revoked key is
 * there. */
atlas_status atlas_db_apikey_link_rotation(atlas_db *db, const char *old_key_id,
                                           const char *new_key_id, atlas_err *err);

/* --- the audit record ----------------------------------------------------- */

typedef enum atlas_gw_interface {
    /* Zero is not a surface. An audit row nobody filled in must not read as
     * having arrived on a particular interface. */
    ATLAS_GW_IFACE_UNKNOWN = 0,
    ATLAS_GW_IFACE_REMOTE_MCP,
    ATLAS_GW_IFACE_WEB_API,
    ATLAS_GW_IFACE_WEB_GUI
} atlas_gw_interface;

const char *atlas_gw_interface_name(atlas_gw_interface i);
atlas_gw_interface atlas_gw_interface_parse(const char *s);

typedef enum atlas_gw_decision {
    /* Zero is DENIED, for the reason A6 keeps BLOCKED there: an audit row that
     * nobody filled in must not claim a request was permitted. */
    ATLAS_GW_DENIED = 0,
    ATLAS_GW_ALLOWED
} atlas_gw_decision;

typedef enum atlas_gw_outcome {
    /* Zero is UNKNOWN. A request whose outcome was never recorded did not
     * succeed as far as this table is concerned. */
    ATLAS_GW_OUTCOME_UNKNOWN = 0,
    ATLAS_GW_OUTCOME_OK,
    ATLAS_GW_OUTCOME_FAILED
} atlas_gw_outcome;

const char *atlas_gw_decision_name(atlas_gw_decision d);
const char *atlas_gw_outcome_name(atlas_gw_outcome o);

typedef struct atlas_gw_audit_entry {
    int64_t id;
    char at[ATLAS_TS_MAX];
    atlas_gw_interface iface;
    /* Empty when the request never authenticated — which is itself the fact
     * being recorded, and is why this is not a foreign key. */
    char key_id[ATLAS_APIKEY_SELECTOR_HEX + 1];
    char label[ATLAS_APIKEY_LABEL_MAX + 1];
    char operation[ATLAS_GW_AUDIT_OP_MAX];
    atlas_gw_decision decision;
    atlas_gw_outcome outcome;
    int32_t status; /* the Atlas exit-code vocabulary, so there is only one */
    int64_t duration_ms;
    char detail[ATLAS_GW_AUDIT_DETAIL_MAX];
} atlas_gw_audit_entry;

void atlas_gw_audit_entry_init(atlas_gw_audit_entry *e);

/* Appends one audit row.
 *
 * `detail` and `operation` are safe-encoded by the caller before they arrive.
 * That is the defence against audit-log injection: a crafted tool name or error
 * message must not be able to forge what looks like a second row, and the
 * encoding is reversible so nothing is lost.
 *
 * **A failure here is reported and never propagated into the request.** A9.6
 * requires that audit failure does not break request handling, so the caller
 * logs and continues. That is a deliberate trade and it is stated in
 * `docs/remote-access.md`: Atlas prefers answering with a gap in the trail to
 * refusing a request because it could not write one. */
atlas_status atlas_db_gw_audit_append(atlas_db *db, const atlas_gw_audit_entry *e, atlas_err *err);

/* The most recent audit rows, newest first.
 *
 * `before_id` pages backwards through the AUTOINCREMENT id and is zero for the
 * first page. `limit` is clamped at ATLAS_GW_AUDIT_MAX_ROWS by the caller and
 * reaching it is reported through `*more_out` rather than being silent. */
typedef atlas_status (*atlas_gw_audit_row_cb)(const atlas_gw_audit_entry *e, void *ud,
                                              atlas_err *err);
atlas_status atlas_db_gw_audit_list(atlas_db *db, int64_t limit, int64_t before_id,
                                    const char *key_id_filter, atlas_gw_audit_row_cb cb, void *ud,
                                    int64_t *count_out, bool *more_out, atlas_err *err);

/* --- the credential service ----------------------------------------------- */

typedef struct atlas_apikey_create_opts {
    const char *label;
    atlas_scope_mask scopes;
    /* The key this one replaces, or NULL/"" for a plain create. Rotation is
     * create-then-revoke inside one transaction, so there is never a moment
     * when neither key works. */
    const char *rotate_from;
} atlas_apikey_create_opts;

/* The result of a create, and the only place the plaintext ever exists.
 *
 * The caller prints `token` once and then calls `atlas_apikey_created_free`,
 * which wipes it. Nothing else in Atlas has a field that could hold it: it is
 * not returned by any read, not stored in any column, and not written to any
 * log or audit row. */
typedef struct atlas_apikey_created {
    char key_id[ATLAS_APIKEY_SELECTOR_HEX + 1];
    char token[ATLAS_APIKEY_TOKEN_MAX];
    char label[ATLAS_APIKEY_LABEL_MAX + 1];
    char scopes[ATLAS_APIKEY_SCOPES_MAX];
    char created_at[ATLAS_TS_MAX];
    char rotated_from[ATLAS_APIKEY_SELECTOR_HEX + 1];
    /* True when the rotation actually revoked the previous key. False with a
     * non-empty `rotated_from` means it was already revoked, which is reported
     * rather than treated as success. */
    bool previous_revoked;
} atlas_apikey_created;

void atlas_apikey_created_free(atlas_apikey_created *c);

/* A listing, materialised so a renderer never holds a borrowed row. */
typedef struct atlas_apikey_listing {
    atlas_apikey_record *keys;
    size_t count;
} atlas_apikey_listing;

void atlas_apikey_listing_init(atlas_apikey_listing *l);
void atlas_apikey_listing_free(atlas_apikey_listing *l);

/* The core operations, over a handle the caller already owns.
 *
 * Each owns its own transaction, so none of these may be called from inside
 * one. The local CLI path below opens a handle and takes the writer lock; the
 * daemon calls these from the writer thread, which already is the single
 * writer. One implementation, for the reason `atlas_maintenance_on` is one. */
atlas_status atlas_apikey_create_on(atlas_db *db, const atlas_apikey_create_opts *opts,
                                    atlas_apikey_created *out, atlas_err *err);
atlas_status atlas_apikey_revoke_on(atlas_db *db, const char *key_id, bool *changed,
                                    atlas_err *err);
atlas_status atlas_apikey_list_on(atlas_db *db, atlas_apikey_listing *out, atlas_err *err);

/* Local entry points. These open the index and, for the writes, take the
 * data-directory writer lock exclusively — which is what makes "the daemon must
 * be stopped" a fact the kernel enforces rather than an instruction in a
 * manual, exactly as A5 does for restore and prune.
 *
 * Under a separated A7.1 deployment the operator uid cannot open the index at
 * all, and these fail with the filesystem's own refusal. That is why the same
 * operations exist as operator-uid RPC methods: the local path is the bootstrap
 * and the single-account case, and the socket is the separated one. */
atlas_status atlas_service_apikey_create(const char *data_dir_override,
                                         const atlas_apikey_create_opts *opts,
                                         atlas_apikey_created *out, atlas_err *err);
atlas_status atlas_service_apikey_revoke(const char *data_dir_override, const char *key_id,
                                         bool *changed, atlas_err *err);
atlas_status atlas_service_apikey_list(const char *data_dir_override, atlas_apikey_listing *out,
                                       atlas_err *err);

/* Validates a key id as an operator may type it.
 *
 * Accepts the bare selector and the `key_` display prefix, so the id printed by
 * `create` can be pasted straight into `revoke`. Refuses anything else: an id is
 * 16 lowercase hex characters, and guessing what a caller meant is how a
 * protocol grows undocumented behaviour. `out` must hold
 * ATLAS_APIKEY_SELECTOR_HEX + 1 bytes. */
bool atlas_apikey_id_normalise(const char *given, char *out);

/* The `key_` prefix the CLI displays. Not part of the token and not stored: it
 * exists so a key id is recognisable in a terminal and in a ticket. */
#define ATLAS_APIKEY_ID_PREFIX "key_"

#endif /* ATLAS_GW_H */

/* Atlas - A9 storage: remote credentials and the gateway audit trail.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See include/atlas/gw.h. Every statement below is a string literal, because
 * `atlas_db_prepare` caches by SQL pointer and confirms the text — a caller
 * that formats SQL into a reused buffer is the one thing that cache is
 * documented to punish.
 */
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/gw.h"
#include "db/db_internal.h"

/* --- vocabularies --------------------------------------------------------- */

const char *atlas_gw_interface_name(atlas_gw_interface i) {
    switch (i) {
    case ATLAS_GW_IFACE_REMOTE_MCP:
        return "REMOTE_MCP";
    case ATLAS_GW_IFACE_WEB_API:
        return "WEB_API";
    case ATLAS_GW_IFACE_WEB_GUI:
        return "WEB_GUI";
    case ATLAS_GW_IFACE_UNKNOWN:
        break;
    }
    return "UNKNOWN";
}

atlas_gw_interface atlas_gw_interface_parse(const char *s) {
    if (s == NULL) {
        return ATLAS_GW_IFACE_UNKNOWN;
    }
    if (strcmp(s, "REMOTE_MCP") == 0) {
        return ATLAS_GW_IFACE_REMOTE_MCP;
    }
    if (strcmp(s, "WEB_API") == 0) {
        return ATLAS_GW_IFACE_WEB_API;
    }
    if (strcmp(s, "WEB_GUI") == 0) {
        return ATLAS_GW_IFACE_WEB_GUI;
    }
    return ATLAS_GW_IFACE_UNKNOWN;
}

const char *atlas_gw_decision_name(atlas_gw_decision d) {
    return d == ATLAS_GW_ALLOWED ? "ALLOWED" : "DENIED";
}

const char *atlas_gw_outcome_name(atlas_gw_outcome o) {
    switch (o) {
    case ATLAS_GW_OUTCOME_OK:
        return "OK";
    case ATLAS_GW_OUTCOME_FAILED:
        return "FAILED";
    case ATLAS_GW_OUTCOME_UNKNOWN:
        break;
    }
    return "UNKNOWN";
}

void atlas_gw_audit_entry_init(atlas_gw_audit_entry *e) {
    memset(e, 0, sizeof(*e));
    /* The zeroes already mean DENIED and UNKNOWN. Stated here so that reading
     * this function does not leave anybody wondering. */
}

/* --- small helpers -------------------------------------------------------- */

static void copy_text(char *dst, size_t dst_size, const unsigned char *src) {
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen((const char *)src);
    if (n >= dst_size) {
        n = dst_size - 1u;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Copies a fixed-width blob column, and reports a row whose blob is the wrong
 * size by leaving the field zeroed.
 *
 * A salt or verifier of an unexpected length is not something to work around:
 * `atlas_apikey_verify` refuses any row whose fields are not exactly the
 * documented sizes, so a short read here becomes a credential that matches
 * nothing rather than one that matches everything. */
static bool copy_blob(unsigned char *dst, size_t want, sqlite3_stmt *s, int col) {
    const void *p = sqlite3_column_blob(s, col);
    int n = sqlite3_column_bytes(s, col);
    if (p == NULL || n < 0 || (size_t)n != want) {
        memset(dst, 0, want);
        return false;
    }
    memcpy(dst, p, want);
    return true;
}

/* --- api_keys ------------------------------------------------------------- */

atlas_status atlas_db_apikey_insert(atlas_db *db, const atlas_apikey_record *rec, atlas_err *err) {
    /* No upsert. A credential is created once; an "insert or replace" would let
     * a second create silently take over an existing key id, and the selector
     * is random precisely so that cannot happen by accident. */
    static const char SQL[] =
        "INSERT INTO api_keys(key_id, label, scopes, salt, verifier, kdf, status, created_at,"
        "                     revoked_at, last_used_at, rotated_from, rotated_to)"
        " VALUES(?1, ?2, ?3, ?4, ?5, 'HMAC-SHA256', ?6, ?7, '', '', ?8, '');";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_bind_text_opt(db, stmt, 1, rec->key_id, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 2, rec->label, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, rec->scopes, err);
    }
    if (st == ATLAS_OK) {
        if (sqlite3_bind_blob(stmt, 4, rec->salt, (int)sizeof rec->salt, SQLITE_TRANSIENT) !=
                SQLITE_OK ||
            sqlite3_bind_blob(stmt, 5, rec->verifier, (int)sizeof rec->verifier, SQLITE_TRANSIENT) !=
                SQLITE_OK) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the credential verifier");
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 6, atlas_apikey_status_name(rec->status), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 7, rec->created_at, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 8, rec->rotated_from, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

/* Fills a record from a statement positioned on a row.
 *
 * The two queries below select the same twelve columns in the same order, and
 * both read them through here. The column list is repeated in each literal
 * rather than assembled from a shared fragment because `atlas_db_prepare`
 * caches by SQL pointer: a query built at runtime presents the same address
 * with different text and is handed the previous statement. One reader over two
 * identical lists is the compromise — a list that drifted would be caught here
 * by reading the wrong column, which the tests exercise. */
static void read_record(sqlite3_stmt *s, atlas_apikey_record *out) {
    memset(out, 0, sizeof(*out));
    out->id = sqlite3_column_int64(s, 0);
    copy_text(out->key_id, sizeof out->key_id, sqlite3_column_text(s, 1));
    copy_text(out->label, sizeof out->label, sqlite3_column_text(s, 2));
    copy_text(out->scopes, sizeof out->scopes, sqlite3_column_text(s, 3));
    bool salt_ok = copy_blob(out->salt, sizeof out->salt, s, 4);
    bool ver_ok = copy_blob(out->verifier, sizeof out->verifier, s, 5);
    copy_text(out->created_at, sizeof out->created_at, sqlite3_column_text(s, 7));
    copy_text(out->revoked_at, sizeof out->revoked_at, sqlite3_column_text(s, 8));
    copy_text(out->last_used_at, sizeof out->last_used_at, sqlite3_column_text(s, 9));
    copy_text(out->rotated_from, sizeof out->rotated_from, sqlite3_column_text(s, 10));
    copy_text(out->rotated_to, sizeof out->rotated_to, sqlite3_column_text(s, 11));

    {
        const unsigned char *t = sqlite3_column_text(s, 6);
        out->status = atlas_apikey_status_parse(t == NULL ? NULL : (const char *)t);
    }
    /* A status the CHECK constraint should have made impossible, or a
     * mis-shaped verifier, makes the key unusable rather than differently
     * usable. UNKNOWN authorises nothing, and `scopes_unreadable` is what the
     * caller reports. */
    if (!salt_ok || !ver_ok) {
        out->status = ATLAS_APIKEY_STATUS_UNKNOWN;
        out->scopes_unreadable = true;
    }

    atlas_err serr;
    atlas_err_init(&serr);
    if (atlas_apikey_scopes_parse(out->scopes, &out->mask, &serr) != ATLAS_OK) {
        /* An older Atlas reading a row a newer one wrote. Fail closed: the mask
         * is zero and the caller is told it could not be read, which is a
         * different thing from a credential that grants nothing. */
        out->mask = 0u;
        out->scopes_unreadable = true;
    }
}

atlas_status atlas_db_apikey_lookup(atlas_db *db, const char *key_id, atlas_apikey_record *out,
                                    bool *found, atlas_err *err) {
    memset(out, 0, sizeof(*out));
    *found = false;
    static const char SQL[] =
        "SELECT id, key_id, label, scopes, salt, verifier, status, created_at, revoked_at,"
        "       last_used_at, rotated_from, rotated_to FROM api_keys WHERE key_id = ?1;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_bind_text_opt(db, stmt, 1, key_id, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        read_record(stmt, out);
        *found = true;
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read the credential");
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_apikey_list(atlas_db *db, atlas_apikey_row_cb cb, void *ud,
                                  int64_t *count_out, atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    static const char SQL[] =
        "SELECT id, key_id, label, scopes, salt, verifier, status, created_at, revoked_at,"
        "       last_used_at, rotated_from, rotated_to FROM api_keys ORDER BY id DESC LIMIT ?1;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, (int64_t)ATLAS_APIKEY_MAX_KEYS) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the credential ceiling");
    }
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        atlas_apikey_record rec;
        read_record(stmt, &rec);
        if (count_out != NULL) {
            (*count_out)++;
        }
        if (cb != NULL) {
            st = cb(&rec, ud, err);
            if (st != ATLAS_OK) {
                break;
            }
        }
    }
    if (st == ATLAS_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot list credentials");
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_apikey_count(atlas_db *db, int64_t *out, atlas_err *err) {
    return atlas_db_query_int64(db, "SELECT count(*) FROM api_keys;", out, err);
}

atlas_status atlas_db_apikey_revoke(atlas_db *db, const char *key_id, bool *changed,
                                    atlas_err *err) {
    *changed = false;
    /* The UPDATE names the state it observed, which is A4's rule: a concurrent
     * revoke loses deterministically instead of last-write-wins, and "already
     * revoked" is distinguishable from "revoked by me" rather than both looking
     * like success. */
    static const char SQL[] = "UPDATE api_keys SET status = 'REVOKED', revoked_at = ?2"
                              " WHERE key_id = ?1 AND status = 'ACTIVE';";
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof now);
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_bind_text_opt(db, stmt, 1, key_id, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 2, now, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    st = atlas_db_step_done(db, stmt, err);
    if (st == ATLAS_OK) {
        *changed = sqlite3_changes(db->h) == 1;
    }
    return st;
}

atlas_status atlas_db_apikey_touch(atlas_db *db, const char *key_id, atlas_err *err) {
    /* Throttled in SQL rather than in the caller, so two gateway processes
     * cannot each decide independently that it is their turn to write. */
    static const char SQL[] = "UPDATE api_keys SET last_used_at = ?2"
                              " WHERE key_id = ?1 AND status = 'ACTIVE' AND last_used_at < ?3;";
    char now[ATLAS_TS_MAX];
    char floor[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof now);
    atlas_iso8601_before_now(floor, sizeof floor, ATLAS_APIKEY_TOUCH_INTERVAL_MS);
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_bind_text_opt(db, stmt, 1, key_id, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 2, now, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, floor, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_apikey_link_rotation(atlas_db *db, const char *old_key_id,
                                           const char *new_key_id, atlas_err *err) {
    /* Only the forward link is written here; the new key's `rotated_from` was
     * set at insert. Two statements would need a transaction, and the caller
     * already owns one — this runs inside it. */
    static const char SQL[] = "UPDATE api_keys SET rotated_to = ?2 WHERE key_id = ?1;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_bind_text_opt(db, stmt, 1, old_key_id, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 2, new_key_id, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

/* --- gw_audit ------------------------------------------------------------- */

atlas_status atlas_db_gw_audit_append(atlas_db *db, const atlas_gw_audit_entry *e, atlas_err *err) {
    static const char SQL[] =
        "INSERT INTO gw_audit(at, interface, key_id, label, operation, decision, outcome,"
        "                     status, duration_ms, detail)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10);";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char now[ATLAS_TS_MAX];
    if (e->at[0] == '\0') {
        atlas_now_iso8601(now, sizeof now);
    } else {
        (void)snprintf(now, sizeof now, "%s", e->at);
    }
    /* An interface the caller left zeroed would fail the CHECK, which would
     * turn a missing field into a failed insert rather than into a row that
     * quietly claims a surface. Recorded as REMOTE_MCP is *not* the fallback;
     * the insert is refused. */
    const char *iface = atlas_gw_interface_name(e->iface);
    st = atlas_db_bind_text_opt(db, stmt, 1, now, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 2, iface, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, e->key_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 4, e->label, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 5, e->operation, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 6, atlas_gw_decision_name(e->decision), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 7, atlas_gw_outcome_name(e->outcome), err);
    }
    if (st == ATLAS_OK) {
        if (sqlite3_bind_int(stmt, 8, (int)e->status) != SQLITE_OK ||
            sqlite3_bind_int64(stmt, 9, e->duration_ms) != SQLITE_OK) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind audit measurements");
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 10, e->detail, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_gw_audit_list(atlas_db *db, int64_t limit, int64_t before_id,
                                    const char *key_id_filter, atlas_gw_audit_row_cb cb, void *ud,
                                    int64_t *count_out, bool *more_out, atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    if (more_out != NULL) {
        *more_out = false;
    }
    if (limit <= 0 || limit > ATLAS_GW_AUDIT_MAX_ROWS) {
        limit = ATLAS_GW_AUDIT_MAX_ROWS;
    }

    /* Two literals rather than one string built at runtime. `atlas_db_prepare`
     * caches by SQL pointer, so a formatted query would present the same
     * address with different text and be handed the previous statement — the
     * defect this repository has already had once. */
    static const char SQL_ALL[] =
        "SELECT id, at, interface, key_id, label, operation, decision, outcome, status,"
        "       duration_ms, detail FROM gw_audit"
        " WHERE (?2 = 0 OR id < ?2) ORDER BY id DESC LIMIT ?1;";
    static const char SQL_KEY[] =
        "SELECT id, at, interface, key_id, label, operation, decision, outcome, status,"
        "       duration_ms, detail FROM gw_audit"
        " WHERE key_id = ?3 AND (?2 = 0 OR id < ?2) ORDER BY id DESC LIMIT ?1;";
    bool filtered = key_id_filter != NULL && key_id_filter[0] != '\0';

    sqlite3_stmt *stmt = NULL;
    /* One row over the limit, so "there is more" is observed rather than
     * inferred from a full page — a page that happens to be exactly `limit`
     * long is not evidence of anything. */
    atlas_status st = atlas_db_prepare(db, filtered ? SQL_KEY : SQL_ALL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, limit + 1) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, before_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind audit bounds");
    }
    if (filtered) {
        st = atlas_db_bind_text_opt(db, stmt, 3, key_id_filter, err);
        if (st != ATLAS_OK) {
            atlas_db_finish(db, stmt);
            return st;
        }
    }

    int64_t seen = 0;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (seen == limit) {
            if (more_out != NULL) {
                *more_out = true;
            }
            break;
        }
        atlas_gw_audit_entry e;
        atlas_gw_audit_entry_init(&e);
        e.id = sqlite3_column_int64(stmt, 0);
        copy_text(e.at, sizeof e.at, sqlite3_column_text(stmt, 1));
        {
            const unsigned char *t = sqlite3_column_text(stmt, 2);
            e.iface = atlas_gw_interface_parse(t == NULL ? NULL : (const char *)t);
        }
        copy_text(e.key_id, sizeof e.key_id, sqlite3_column_text(stmt, 3));
        copy_text(e.label, sizeof e.label, sqlite3_column_text(stmt, 4));
        copy_text(e.operation, sizeof e.operation, sqlite3_column_text(stmt, 5));
        {
            const unsigned char *t = sqlite3_column_text(stmt, 6);
            e.decision = (t != NULL && strcmp((const char *)t, "ALLOWED") == 0) ? ATLAS_GW_ALLOWED
                                                                               : ATLAS_GW_DENIED;
        }
        {
            const unsigned char *t = sqlite3_column_text(stmt, 7);
            if (t != NULL && strcmp((const char *)t, "OK") == 0) {
                e.outcome = ATLAS_GW_OUTCOME_OK;
            } else if (t != NULL && strcmp((const char *)t, "FAILED") == 0) {
                e.outcome = ATLAS_GW_OUTCOME_FAILED;
            } else {
                e.outcome = ATLAS_GW_OUTCOME_UNKNOWN;
            }
        }
        e.status = (int32_t)sqlite3_column_int(stmt, 8);
        e.duration_ms = sqlite3_column_int64(stmt, 9);
        copy_text(e.detail, sizeof e.detail, sqlite3_column_text(stmt, 10));

        seen++;
        if (count_out != NULL) {
            *count_out = seen;
        }
        if (cb != NULL) {
            st = cb(&e, ud, err);
            if (st != ATLAS_OK) {
                break;
            }
        }
    }
    if (st == ATLAS_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot list audit records");
    }
    atlas_db_finish(db, stmt);
    return st;
}

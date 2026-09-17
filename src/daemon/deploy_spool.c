/* Atlas - A17 T2: the queue-file spool and the `.res` parser.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See include/atlas/deploy_spool.h for the contract. This file opens no
 * database transaction of its own kind that is not already documented there,
 * forks no process, and every path it touches is composed from the data
 * directory this daemon was started with plus a deploy uid this process
 * validated the shape of -- `d` plus 32 lowercase hex -- before it was ever
 * used to build one.
 */
#define _GNU_SOURCE 1

#include "atlas/deploy_spool.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "atlas/buf.h"
#include "atlas/orch_ops.h" /* ATLAS_ORCH_RESULT_PATCH_NAME */
#include "atlas/sha256.h"

/* --- small helpers ---------------------------------------------------------- */

static bool is_lower_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

/* `d` plus ATLAS_DEPLOY_UID_HEX lowercase hex, and nothing else -- the shape
 * `atlas_db_deploy_propose_in_tx` itself produces. Never trusts a caller's
 * claim of validity; every function below that composes a path from a uid
 * checks this first. */
static bool deploy_uid_is_valid(const char *uid) {
    if (uid == NULL || uid[0] != 'd') {
        return false;
    }
    size_t n = strlen(uid);
    if (n != 1u + ATLAS_DEPLOY_UID_HEX) {
        return false;
    }
    for (size_t i = 1; i < n; i++) {
        if (!is_lower_hex(uid[i])) {
            return false;
        }
    }
    return true;
}

static void log_line(FILE *log, const char *fmt, ...) {
    if (log == NULL) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    (void)fprintf(log, "atlas: deploy: ");
    (void)vfprintf(log, fmt, ap);
    (void)fprintf(log, "\n");
    (void)fflush(log);
    va_end(ap);
}

static atlas_status deploy_dir_path(const char *data_dir, atlas_buf *out, atlas_err *err) {
    atlas_buf_reset(out);
    return atlas_buf_appendf(out, err, "%s/deploy", data_dir);
}

static atlas_status deploy_subdir_path(const char *data_dir, const char *sub, atlas_buf *out,
                                       atlas_err *err) {
    atlas_buf_reset(out);
    return atlas_buf_appendf(out, err, "%s/deploy/%s", data_dir, sub);
}

/* --- directories ------------------------------------------------------------ */

static atlas_status mkdir_0700(const char *path, atlas_err *err) {
    if (mkdir(path, S_IRWXU) != 0 && errno != EEXIST) {
        return atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno,
                                   "cannot create the Atlas deploy spool directory %s", path);
    }
    return ATLAS_OK;
}

atlas_status atlas_deploy_spool_ensure_dirs(const char *data_dir, atlas_err *err) {
    atlas_buf p = ATLAS_BUF_INIT;
    atlas_status st = deploy_dir_path(data_dir, &p, err);
    if (st == ATLAS_OK) {
        st = mkdir_0700(atlas_buf_cstr(&p), err);
    }
    if (st == ATLAS_OK) {
        st = deploy_subdir_path(data_dir, "requests", &p, err);
    }
    if (st == ATLAS_OK) {
        st = mkdir_0700(atlas_buf_cstr(&p), err);
    }
    if (st == ATLAS_OK) {
        st = deploy_subdir_path(data_dir, "results", &p, err);
    }
    if (st == ATLAS_OK) {
        st = mkdir_0700(atlas_buf_cstr(&p), err);
    }
    atlas_buf_free(&p);
    return st;
}

/* --- "is results/ non-empty?" ------------------------------------------------ */

/* Exactly `d` + 32 lowercase hex + ".res" -- never a `.tmp.XXXXXX` fragment
 * `write_result`'s own tmp-then-rename write leaves mid-write, and never a
 * `.bad`. */
static bool is_res_filename(const char *name) {
    size_t n = strlen(name);
    size_t want = 1u + ATLAS_DEPLOY_UID_HEX + 4u; /* "d" + hex + ".res" */
    if (n != want || name[0] != 'd') {
        return false;
    }
    for (size_t i = 1; i <= ATLAS_DEPLOY_UID_HEX; i++) {
        if (!is_lower_hex(name[i])) {
            return false;
        }
    }
    return strcmp(name + 1u + ATLAS_DEPLOY_UID_HEX, ".res") == 0;
}

bool atlas_deploy_spool_results_pending(const char *data_dir) {
    atlas_buf p = ATLAS_BUF_INIT;
    atlas_err ignore;
    atlas_err_init(&ignore);
    if (deploy_subdir_path(data_dir, "results", &p, &ignore) != ATLAS_OK) {
        atlas_buf_free(&p);
        return false;
    }
    DIR *d = opendir(atlas_buf_cstr(&p));
    atlas_buf_free(&p);
    if (d == NULL) {
        /* No directory: nothing is owed. A daemon that has not yet run
         * `atlas_deploy_spool_ensure_dirs`, or a hand-edited data directory,
         * is an ordinary state here, not a fault. */
        return false;
    }
    bool pending = false;
    struct dirent *de;
    while (!pending && (de = readdir(d)) != NULL) {
        if (is_res_filename(de->d_name)) {
            pending = true;
        }
    }
    (void)closedir(d);
    return pending;
}

/* --- writing the two queue files for one deploy ------------------------------ */

/* Writes `data` to `dir/name`, tmp then rename within the same directory so
 * the rename is atomic on any filesystem this daemon's data directory lives
 * on. The tmp name carries a `.tmp` suffix that cannot match `*.req`, so a
 * path unit watching for a finished request file never observes the
 * half-written form. Mode 0600: only this uid needs to read it, and the root
 * agent runs as a different uid entirely -- reading it back is `open(2)` by
 * a process with the authority to, not a permission this file grants. */
static atlas_status write_atomic_in(const char *dir, const char *name, const void *data,
                                    size_t len, atlas_err *err) {
    atlas_buf final_path = ATLAS_BUF_INIT;
    atlas_buf tmp_path = ATLAS_BUF_INIT;
    atlas_status st = atlas_buf_appendf(&final_path, err, "%s/%s", dir, name);
    if (st == ATLAS_OK) {
        st = atlas_buf_appendf(&tmp_path, err, "%s/%s.tmp", dir, name);
    }
    if (st != ATLAS_OK) {
        atlas_buf_free(&final_path);
        atlas_buf_free(&tmp_path);
        return st;
    }
    const char *tmp = atlas_buf_cstr(&tmp_path);
    (void)unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        st = atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno, "cannot create %s", tmp);
        atlas_buf_free(&final_path);
        atlas_buf_free(&tmp_path);
        return st;
    }
    size_t off = 0;
    while (off < len) {
        ssize_t wn = write(fd, (const char *)data + off, len - off);
        if (wn < 0) {
            if (errno == EINTR) {
                continue;
            }
            st = atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno, "cannot write %s", tmp);
            break;
        }
        off += (size_t)wn;
    }
    if (st == ATLAS_OK && fsync(fd) != 0) {
        st = atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno, "cannot flush %s", tmp);
    }
    (void)close(fd);
    if (st == ATLAS_OK && rename(tmp, atlas_buf_cstr(&final_path)) != 0) {
        st = atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno, "cannot install %s",
                                 atlas_buf_cstr(&final_path));
    }
    if (st != ATLAS_OK) {
        (void)unlink(tmp);
    }
    atlas_buf_free(&final_path);
    atlas_buf_free(&tmp_path);
    return st;
}

/* The single stored artifact this deploy's job carries: `changes.patch`,
 * inline content, or nothing at all -- T1's `propose_in_tx` already refused a
 * job whose artifact was not stored or was empty, so a miss here (the job
 * lost its artifact between propose and confirm, which nothing in Atlas ever
 * does) is treated as an ordinary spool failure rather than a database
 * error. */
typedef struct patch_slot {
    bool found;
    atlas_buf content;
    char sha256[ATLAS_SHA256_HEX_LEN + 1u];
} patch_slot;

static atlas_status take_patch_artifact(const atlas_orch_artifact_row *row, void *ud,
                                        atlas_err *err) {
    patch_slot *s = (patch_slot *)ud;
    if (row->name == NULL || strcmp(row->name, ATLAS_ORCH_RESULT_PATCH_NAME) != 0) {
        return ATLAS_OK;
    }
    if (!row->content_stored || row->content == NULL) {
        return ATLAS_OK;
    }
    s->found = true;
    (void)snprintf(s->sha256, sizeof s->sha256, "%s", row->sha256 != NULL ? row->sha256 : "");
    return atlas_buf_set(&s->content, row->content, row->content_len, err);
}

atlas_status atlas_deploy_spool_write_one(atlas_db *db, const char *data_dir,
                                          const char *deploy_uid, FILE *log, bool *spooled_out,
                                          atlas_err *err) {
    if (spooled_out != NULL) {
        *spooled_out = false;
    }
    if (!deploy_uid_is_valid(deploy_uid)) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "not a deploy uid: %s", deploy_uid);
    }

    atlas_deploy_row row;
    atlas_deploy_row_init(&row);
    bool have = false;
    atlas_status st = atlas_db_deploy_get(db, deploy_uid, &row, &have, err);
    if (st != ATLAS_OK) {
        atlas_deploy_row_free(&row);
        return st;
    }
    if (!have || row.state != ATLAS_DEPLOY_CONFIRMED) {
        /* Nothing to spool: already spooled and moved on, cancelled, or this
         * uid never existed. Not an error -- the caller decided this uid was
         * worth trying. */
        atlas_deploy_row_free(&row);
        return ATLAS_OK;
    }

    atlas_repo_info ri;
    atlas_repo_info_init(&ri);
    bool repo_found = false;
    st = atlas_db_repo_get_by_id(db, row.repo_id, &ri, &repo_found, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&ri);
        atlas_deploy_row_free(&row);
        return st;
    }
    if (!repo_found) {
        log_line(log, "deploy %s names repository %lld, which no longer resolves; not spooled",
                deploy_uid, (long long)row.repo_id);
        atlas_repo_info_free(&ri);
        atlas_deploy_row_free(&row);
        return ATLAS_OK;
    }

    patch_slot slot;
    memset(&slot, 0, sizeof slot);
    atlas_buf_init(&slot.content);
    int64_t artifact_n = 0;
    st = atlas_db_orch_artifacts(db, atlas_buf_cstr(&row.job_uid), 0, true, take_patch_artifact,
                                 &slot, &artifact_n, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&slot.content);
        atlas_repo_info_free(&ri);
        atlas_deploy_row_free(&row);
        return st;
    }
    if (!slot.found || strcmp(slot.sha256, atlas_buf_cstr(&row.patch_digest)) != 0) {
        log_line(log,
                "deploy %s's job %s no longer carries the patch this deploy was proposed from "
                "(found=%d); not spooled",
                deploy_uid, atlas_buf_cstr(&row.job_uid), (int)slot.found);
        atlas_buf_free(&slot.content);
        atlas_repo_info_free(&ri);
        atlas_deploy_row_free(&row);
        return ATLAS_OK;
    }

    atlas_buf requests_dir = ATLAS_BUF_INIT;
    atlas_err werr;
    atlas_err_init(&werr);
    atlas_status wst = deploy_subdir_path(data_dir, "requests", &requests_dir, &werr);

    char patch_name[ATLAS_DEPLOY_UID_MAX + 8u];
    (void)snprintf(patch_name, sizeof patch_name, "%s.patch", deploy_uid);
    char req_name[ATLAS_DEPLOY_UID_MAX + 8u];
    (void)snprintf(req_name, sizeof req_name, "%s.req", deploy_uid);

    /* `.patch` before `.req`: the root agent's path unit watches `*.req`
     * alone, so the request is not "visible" until both files are already on
     * disk. */
    if (wst == ATLAS_OK) {
        wst = write_atomic_in(atlas_buf_cstr(&requests_dir), patch_name, slot.content.data,
                              slot.content.len, &werr);
    }
    if (wst == ATLAS_OK) {
        atlas_buf req_body = ATLAS_BUF_INIT;
        char confirmed_at[ATLAS_TS_MAX];
        (void)snprintf(confirmed_at, sizeof confirmed_at, "%s", atlas_buf_cstr(&row.confirmed_at));
        wst = atlas_buf_appendf(
            &req_body, &werr,
            "atlas-deploy-request 1\n"
            "deploy %s\n"
            "job %s\n"
            "repo_root %s\n"
            "base_commit %s\n"
            "patch_sha256 %s\n"
            "patch_bytes %lld\n"
            "confirmed_by %s\n"
            "confirmed_at %s\n",
            deploy_uid, atlas_buf_cstr(&row.job_uid), atlas_buf_cstr(&ri.root_path),
            atlas_buf_cstr(&row.base_commit), atlas_buf_cstr(&row.patch_digest),
            (long long)row.patch_bytes, atlas_buf_cstr(&row.confirmed_key_id), confirmed_at);
        if (wst == ATLAS_OK) {
            wst = write_atomic_in(atlas_buf_cstr(&requests_dir), req_name, req_body.data,
                                  req_body.len, &werr);
        }
        atlas_buf_free(&req_body);
    }
    atlas_buf_free(&requests_dir);
    atlas_buf_free(&slot.content);
    atlas_repo_info_free(&ri);
    atlas_deploy_row_free(&row);

    if (wst != ATLAS_OK) {
        log_line(log, "deploy %s: cannot write the queue request: %s", deploy_uid,
                atlas_err_msg(&werr));
        return ATLAS_OK;
    }

    atlas_status tst = atlas_db_begin(db, err);
    if (tst != ATLAS_OK) {
        return tst;
    }
    tst = atlas_db_deploy_mark_spooled_in_tx(db, deploy_uid, err);
    if (tst == ATLAS_OK) {
        atlas_status cst = atlas_db_commit(db, err);
        if (cst != ATLAS_OK) {
            atlas_db_rollback(db);
            tst = cst;
        }
    } else {
        atlas_db_rollback(db);
    }
    if (tst != ATLAS_OK) {
        /* The queue files are on disk but the row could not be marked
         * spooled -- a database error, not a filesystem one. Reported: this
         * is the one case the caller should hear about, because retrying
         * will write the same two files again over the ones already there,
         * which is harmless but worth knowing happens. */
        log_line(log, "deploy %s: queue files written but the row could not be marked spooled: %s",
                deploy_uid, atlas_err_msg(err));
        return tst;
    }
    if (spooled_out != NULL) {
        *spooled_out = true;
    }
    return ATLAS_OK;
}

atlas_status atlas_deploy_spool_sweep(atlas_db *db, const char *data_dir, FILE *log,
                                      atlas_err *err) {
    atlas_buf uids = ATLAS_BUF_INIT;
    atlas_status st = atlas_db_deploy_confirmed_unspooled(db, &uids, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&uids);
        return st;
    }
    const size_t stride = ATLAS_DEPLOY_UID_HEX + 1u;
    size_t n = uids.len / stride;
    for (size_t i = 0; i < n; i++) {
        char uid[ATLAS_DEPLOY_UID_MAX];
        memset(uid, 0, sizeof uid);
        memcpy(uid, (const char *)uids.data + i * stride, stride);
        bool spooled = false;
        atlas_err werr;
        atlas_err_init(&werr);
        (void)atlas_deploy_spool_write_one(db, data_dir, uid, log, &spooled, &werr);
        /* Each uid's own failure is already logged inside
         * `atlas_deploy_spool_write_one`; the sweep continues over the rest
         * rather than abandoning the whole pass on one bad row. */
    }
    atlas_buf_free(&uids);
    return ATLAS_OK;
}

/* --- parsing a `.res` file ---------------------------------------------------- */

typedef enum res_key {
    RES_DEPLOY = 0,
    RES_OUTCOME,
    RES_STAGE,
    RES_DRY_RUN,
    RES_HEAD_BEFORE,
    RES_HEAD_AFTER,
    RES_TREE_DIRTY_BEFORE,
    RES_INSTALLED_VERSION,
    RES_UNITS,
    RES_ROLLBACK,
    RES_TEXT_BYTES,
    RES_KEY_COUNT
} res_key;

static const char *const RES_KEY_NAMES[RES_KEY_COUNT] = {
    "deploy",  "outcome",   "stage",    "dry_run",         "head_before",
    "head_after", "tree_dirty_before", "installed_version", "units", "rollback",
    "text_bytes"};

/* Required per D.3's own escape hatch (§F): the hand-written ABANDONED result
 * sets only these four (plus `text_bytes 0` for an empty body). Every other
 * key is optional and defaults to `""` (or `no` for the two yes/no fields). */
static bool res_key_required(res_key k) {
    return k == RES_DEPLOY || k == RES_OUTCOME || k == RES_STAGE || k == RES_TEXT_BYTES;
}

static bool res_key_parse(const char *tok, size_t len, res_key *out) {
    for (int k = 0; k < RES_KEY_COUNT; k++) {
        size_t klen = strlen(RES_KEY_NAMES[k]);
        if (klen == len && memcmp(tok, RES_KEY_NAMES[k], len) == 0) {
            *out = (res_key)k;
            return true;
        }
    }
    return false;
}

static bool parse_yes_no(const char *v, size_t vlen, bool *out) {
    if (vlen == 3u && memcmp(v, "yes", 3) == 0) {
        *out = true;
        return true;
    }
    if (vlen == 2u && memcmp(v, "no", 2) == 0) {
        *out = false;
        return true;
    }
    return false;
}

static void copy_bounded(char *dst, size_t dst_size, const char *v, size_t vlen) {
    if (vlen >= dst_size) {
        vlen = dst_size - 1u;
    }
    memcpy(dst, v, vlen);
    dst[vlen] = '\0';
}

static atlas_status result_malformed(bool *ok_out, char *why, size_t why_size, const char *msg) {
    *ok_out = false;
    (void)snprintf(why, why_size, "%s", msg);
    return ATLAS_OK;
}

atlas_status atlas_deploy_result_parse(const char *bytes, size_t len,
                                       atlas_deploy_result_parsed *out, bool *ok_out, char *why,
                                       size_t why_size, atlas_err *err) {
    if (out == NULL || ok_out == NULL || why == NULL || why_size == 0) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "atlas_deploy_result_parse called with a NULL output");
    }
    memset(out, 0, sizeof *out);
    *ok_out = true;
    why[0] = '\0';

    if (len > ATLAS_DEPLOY_RESULT_FILE_MAX) {
        return result_malformed(ok_out, why, why_size, "result malformed: file too large");
    }
    static const char HEADER[] = "atlas-deploy-result 1\n";
    size_t header_len = sizeof(HEADER) - 1u;
    if (len < header_len || memcmp(bytes, HEADER, header_len) != 0) {
        return result_malformed(ok_out, why, why_size,
                                "result malformed: missing atlas-deploy-result header");
    }

    bool seen[RES_KEY_COUNT];
    memset(seen, 0, sizeof seen);
    bool outcome_success = false;
    bool have_outcome = false;
    long long text_bytes = -1;
    bool tree_dirty_before = false; /* parsed, not carried further */
    (void)tree_dirty_before;

    size_t i = header_len;
    bool found_sep = false;
    size_t body_start = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && bytes[i] != '\n') {
            i++;
        }
        size_t end = i;
        if (i < len) {
            i++; /* consume '\n' */
        } else {
            return result_malformed(ok_out, why, why_size,
                                    "result malformed: header line without a newline");
        }
        size_t linelen = end - start;
        if (linelen == 2u && bytes[start] == '-' && bytes[start + 1u] == '-') {
            found_sep = true;
            body_start = i;
            break;
        }
        /* "key value": key up to the first space, value is the rest (may be
         * empty when the line is "key " with nothing after the space). */
        size_t sp = 0;
        while (sp < linelen && bytes[start + sp] != ' ') {
            sp++;
        }
        if (sp == linelen) {
            return result_malformed(ok_out, why, why_size,
                                    "result malformed: header line has no key/value separator");
        }
        res_key key;
        if (!res_key_parse(bytes + start, sp, &key)) {
            return result_malformed(ok_out, why, why_size, "result malformed: unknown key");
        }
        if (seen[key]) {
            return result_malformed(ok_out, why, why_size, "result malformed: key given twice");
        }
        seen[key] = true;
        const char *v = bytes + start + sp + 1u;
        size_t vlen = linelen - sp - 1u;

        switch (key) {
        case RES_DEPLOY: copy_bounded(out->deploy_uid, sizeof out->deploy_uid, v, vlen); break;
        case RES_OUTCOME:
            if (vlen == 9u && memcmp(v, "SUCCEEDED", 9) == 0) {
                outcome_success = true;
            } else if (vlen == 6u && memcmp(v, "FAILED", 6) == 0) {
                outcome_success = false;
            } else {
                return result_malformed(ok_out, why, why_size,
                                        "result malformed: outcome is not SUCCEEDED or FAILED");
            }
            have_outcome = true;
            break;
        case RES_STAGE: copy_bounded(out->stage, sizeof out->stage, v, vlen); break;
        case RES_DRY_RUN:
            if (!parse_yes_no(v, vlen, &out->dry_run)) {
                return result_malformed(ok_out, why, why_size,
                                        "result malformed: dry_run is not yes or no");
            }
            break;
        case RES_HEAD_BEFORE:
            copy_bounded(out->head_before, sizeof out->head_before, v, vlen);
            break;
        case RES_HEAD_AFTER: copy_bounded(out->head_after, sizeof out->head_after, v, vlen); break;
        case RES_TREE_DIRTY_BEFORE:
            if (!parse_yes_no(v, vlen, &tree_dirty_before)) {
                return result_malformed(ok_out, why, why_size,
                                        "result malformed: tree_dirty_before is not yes or no");
            }
            break;
        case RES_INSTALLED_VERSION:
            copy_bounded(out->installed_version, sizeof out->installed_version, v, vlen);
            break;
        case RES_UNITS: /* recorded nowhere; migration 33 has no column for it */ break;
        case RES_ROLLBACK: copy_bounded(out->rollback, sizeof out->rollback, v, vlen); break;
        case RES_TEXT_BYTES: {
            if (vlen == 0 || vlen > 10u) {
                return result_malformed(ok_out, why, why_size,
                                        "result malformed: text_bytes is not a number");
            }
            char numbuf[16];
            memcpy(numbuf, v, vlen);
            numbuf[vlen] = '\0';
            for (size_t k = 0; k < vlen; k++) {
                if (numbuf[k] < '0' || numbuf[k] > '9') {
                    return result_malformed(ok_out, why, why_size,
                                            "result malformed: text_bytes is not a number");
                }
            }
            text_bytes = atoll(numbuf);
            if (text_bytes < 0 || (unsigned long long)text_bytes > ATLAS_DEPLOY_RESULT_TEXT_MAX) {
                return result_malformed(ok_out, why, why_size,
                                        "result malformed: text_bytes out of bounds");
            }
            break;
        }
        case RES_KEY_COUNT:
            /* Never produced by `res_key_parse`, which only ever returns a
             * cast `int` in `[0, RES_KEY_COUNT)`. Listed so `-Wswitch-enum`
             * treats this switch as complete rather than relying on a
             * `default:` this file does not otherwise need. */
            break;
        }
    }
    if (!found_sep) {
        return result_malformed(ok_out, why, why_size, "result malformed: missing \"--\" line");
    }
    for (int k = 0; k < RES_KEY_COUNT; k++) {
        if (res_key_required((res_key)k) && !seen[k]) {
            return result_malformed(ok_out, why, why_size, "result malformed: a required key is missing");
        }
    }
    if (!have_outcome) {
        return result_malformed(ok_out, why, why_size, "result malformed: outcome is missing");
    }
    size_t body_len = len - body_start;
    if ((long long)body_len != text_bytes) {
        return result_malformed(ok_out, why, why_size,
                                "result malformed: text_bytes does not match the body length");
    }
    memcpy(out->text, bytes + body_start, body_len);
    out->text[body_len] = '\0';
    out->text_len = body_len;
    out->success = outcome_success;
    *ok_out = true;
    return ATLAS_OK;
}

/* --- ingest ------------------------------------------------------------------- */

/* `qsort` is over `char names[ATLAS_DEPLOY_INGEST_MAX_FILES][N]` (an array of
 * fixed-width buffers, not an array of pointers), so each element `a`/`b`
 * already points at the filename bytes themselves -- there is no pointer to
 * dereference through, the shape `src/memory/read.c` uses for the same kind
 * of fixed-width buffer sort. The earlier form read the first sizeof(char*)
 * bytes of a filename as a pointer and dereferenced it: a deterministic
 * SIGSEGV on the writer thread whenever two or more `.res` files were
 * pending in one ingest pass. */
static int name_cmp(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

static atlas_status ingest_one_file(atlas_db *db, const char *results_dir, const char *filename,
                                    FILE *log, atlas_err *err) {
    /* The uid this process acts on is always the filename's, never the
     * body's own `deploy` field -- the body is exactly as untrusted as any
     * other bytes a foreign process wrote to a directory Atlas owns. */
    char deploy_uid[ATLAS_DEPLOY_UID_MAX];
    memset(deploy_uid, 0, sizeof deploy_uid);
    memcpy(deploy_uid, filename, ATLAS_DEPLOY_UID_HEX + 1u);

    atlas_buf path = ATLAS_BUF_INIT;
    atlas_status st = atlas_buf_appendf(&path, err, "%s/%s", results_dir, filename);
    if (st != ATLAS_OK) {
        atlas_buf_free(&path);
        return st;
    }

    char buf[ATLAS_DEPLOY_RESULT_FILE_MAX + 1u];
    size_t total = 0;
    bool read_ok = true;
    int fd = open(atlas_buf_cstr(&path), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        /* Gone already (a concurrent run, or an operator cleaned up by
         * hand): nothing to ingest. Not an error. */
        atlas_buf_free(&path);
        return ATLAS_OK;
    }
    for (;;) {
        if (total >= sizeof buf) {
            read_ok = false; /* larger than the bound: malformed by size alone */
            break;
        }
        ssize_t r = read(fd, buf + total, sizeof(buf) - total);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            read_ok = false;
            break;
        }
        if (r == 0) {
            break;
        }
        total += (size_t)r;
    }
    (void)close(fd);

    bool ok = false;
    char why[160];
    why[0] = '\0';
    atlas_deploy_result_parsed parsed;
    if (read_ok) {
        atlas_err perr;
        atlas_err_init(&perr);
        st = atlas_deploy_result_parse(buf, total, &parsed, &ok, why, sizeof why, &perr);
        if (st != ATLAS_OK) {
            atlas_buf_free(&path);
            return st;
        }
    } else {
        ok = false;
        (void)snprintf(why, sizeof why, "result malformed: file too large to read");
        memset(&parsed, 0, sizeof parsed);
    }

    bool filename_agrees = ok && strcmp(parsed.deploy_uid, deploy_uid) == 0;
    if (ok && !filename_agrees) {
        ok = false;
        (void)snprintf(why, sizeof why,
                       "result malformed: the file's own \"deploy\" field does not match its "
                       "filename");
    }

    atlas_deploy_result r;
    memset(&r, 0, sizeof r);
    if (ok) {
        r.success = parsed.success;
        r.stage = parsed.stage;
        r.dry_run = parsed.dry_run;
        r.rollback = parsed.rollback;
        r.head_before = parsed.head_before;
        r.head_after = parsed.head_after;
        r.version = parsed.installed_version;
        r.text = parsed.text;
    } else {
        r.success = false;
        r.stage = "INGEST";
        r.dry_run = false;
        r.rollback = "";
        r.head_before = "";
        r.head_after = "";
        r.version = "";
        r.text = why;
    }

    atlas_err ferr;
    atlas_err_init(&ferr);
    st = atlas_db_begin(db, &ferr);
    if (st == ATLAS_OK) {
        st = atlas_db_deploy_finish_in_tx(db, deploy_uid, &r, &ferr);
        if (st == ATLAS_OK) {
            atlas_status cst = atlas_db_commit(db, &ferr);
            if (cst != ATLAS_OK) {
                atlas_db_rollback(db);
                st = cst;
            }
        } else {
            atlas_db_rollback(db);
        }
    }
    if (st != ATLAS_OK) {
        /* A deploy this uid does not resolve to (never existed, or not
         * CONFIRMED -- already terminal, a double delivery) is not this
         * pass's problem to solve by retrying: the file is unlinked below
         * regardless. */
        log_line(log, "deploy %s: result not applied: %s", deploy_uid, atlas_err_msg(&ferr));
    }

    (void)unlink(atlas_buf_cstr(&path));
    atlas_buf_free(&path);
    return ATLAS_OK;
}

atlas_status atlas_deploy_ingest_pass(atlas_db *db, const char *data_dir, FILE *log,
                                      atlas_err *err) {
    atlas_buf results_dir = ATLAS_BUF_INIT;
    atlas_status st = deploy_subdir_path(data_dir, "results", &results_dir, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&results_dir);
        return st;
    }
    const char *dir_path = atlas_buf_cstr(&results_dir);
    DIR *d = opendir(dir_path);
    if (d == NULL) {
        atlas_buf_free(&results_dir);
        return ATLAS_OK;
    }

    /* Collected first, then sorted and processed -- so a file that appears
     * mid-scan (the agent's tmp-then-rename) is simply not in this pass's
     * list rather than read half-written. Bounded: `ATLAS_DEPLOY_INGEST_MAX_FILES`
     * caps one pass; a directory somehow holding more is finished on the
     * next tick. */
#define ATLAS_DEPLOY_INGEST_MAX_FILES 256u
    char names[ATLAS_DEPLOY_INGEST_MAX_FILES][ATLAS_DEPLOY_UID_MAX + 8u];
    size_t n = 0;
    struct dirent *de;
    while (n < ATLAS_DEPLOY_INGEST_MAX_FILES && (de = readdir(d)) != NULL) {
        if (is_res_filename(de->d_name)) {
            size_t name_len = strlen(de->d_name);
            if (name_len >= sizeof names[n]) {
                continue;
            }
            memcpy(names[n], de->d_name, name_len + 1u);
            n++;
        }
    }
    (void)closedir(d);

    qsort(names, n, sizeof names[0], name_cmp);

    for (size_t i = 0; i < n; i++) {
        atlas_status ist = ingest_one_file(db, dir_path, names[i], log, err);
        if (ist != ATLAS_OK) {
            atlas_buf_free(&results_dir);
            return ist;
        }
    }
    atlas_buf_free(&results_dir);
    return ATLAS_OK;
}

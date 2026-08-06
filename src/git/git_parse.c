/* Atlas - git output parsers.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 */
#include "git/git_parse.h"

#include <stdio.h>
#include <string.h>

/* --- validators ---------------------------------------------------------- */

bool atlas_git_is_hex_oid(const char *s, size_t n) {
    if (n != 40u && n != 64u) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) {
            return false;
        }
    }
    return true;
}

bool atlas_parse_i64(const char *s, size_t n, int64_t *out) {
    if (n == 0 || n > 19u) {
        return false;
    }
    int64_t v = 0;
    size_t i = 0;
    bool neg = false;
    if (s[0] == '-') {
        neg = true;
        i = 1;
        if (n == 1) {
            return false;
        }
    }
    for (; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        v = v * 10 + (int64_t)(s[i] - '0');
    }
    *out = neg ? -v : v;
    return true;
}

/* --- NUL splitter -------------------------------------------------------- */

void atlas_nulsplit_init(atlas_nulsplit *s, size_t max_token, atlas_tok_cb cb, void *ud) {
    atlas_buf_init(&s->acc);
    s->max_token = max_token != 0 ? max_token : ATLAS_GIT_MAX_TOKEN;
    s->cb = cb;
    s->ud = ud;
}

void atlas_nulsplit_free(atlas_nulsplit *s) {
    atlas_buf_free(&s->acc);
}

atlas_status atlas_nulsplit_feed(atlas_nulsplit *s, const char *data, size_t n, atlas_err *err) {
    size_t pos = 0;
    while (pos < n) {
        const char *nul = memchr(data + pos, '\0', n - pos);
        if (nul == NULL) {
            size_t rest = n - pos;
            if (s->acc.len + rest > s->max_token) {
                return atlas_err_set(err, ATLAS_ERR_GIT,
                                     "git record exceeds the %zu byte limit", s->max_token);
            }
            return atlas_buf_append(&s->acc, data + pos, rest, err);
        }
        size_t seg = (size_t)(nul - (data + pos));
        atlas_status st;
        if (s->acc.len == 0) {
            /* Whole record present in this chunk: hand it over without copying. */
            if (seg > s->max_token) {
                return atlas_err_set(err, ATLAS_ERR_GIT, "git record exceeds the %zu byte limit",
                                     s->max_token);
            }
            st = s->cb(data + pos, seg, s->ud, err);
        } else {
            if (s->acc.len + seg > s->max_token) {
                return atlas_err_set(err, ATLAS_ERR_GIT, "git record exceeds the %zu byte limit",
                                     s->max_token);
            }
            st = atlas_buf_append(&s->acc, data + pos, seg, err);
            if (st == ATLAS_OK) {
                st = s->cb(s->acc.data, s->acc.len, s->ud, err);
            }
            atlas_buf_reset(&s->acc);
        }
        if (st != ATLAS_OK) {
            return st;
        }
        pos += seg + 1u;
    }
    return ATLAS_OK;
}

atlas_status atlas_nulsplit_finish(atlas_nulsplit *s, atlas_err *err) {
    if (s->acc.len != 0) {
        return atlas_err_set(err, ATLAS_ERR_GIT,
                             "git output ended mid-record (%zu unterminated bytes)", s->acc.len);
    }
    return ATLAS_OK;
}

/* --- ls-files ------------------------------------------------------------ */

atlas_status atlas_lsfiles_token(const char *tok, size_t len, void *ud, atlas_err *err) {
    atlas_lsfiles_parser *p = (atlas_lsfiles_parser *)ud;
    if (len == 0) {
        return atlas_err_set(err, ATLAS_ERR_GIT, "empty git ls-files record");
    }
    const char *tab = memchr(tok, '\t', len);
    if (tab == NULL) {
        return atlas_err_set(err, ATLAS_ERR_GIT, "git ls-files record has no tab separator");
    }
    size_t head_len = (size_t)(tab - tok);
    const char *sp1 = memchr(tok, ' ', head_len);
    if (sp1 == NULL) {
        return atlas_err_set(err, ATLAS_ERR_GIT, "git ls-files record has no mode separator");
    }
    size_t mode_len = (size_t)(sp1 - tok);
    const char *after_mode = sp1 + 1;
    size_t rest_len = head_len - mode_len - 1u;
    const char *sp2 = memchr(after_mode, ' ', rest_len);
    if (sp2 == NULL) {
        return atlas_err_set(err, ATLAS_ERR_GIT, "git ls-files record has no stage separator");
    }
    size_t oid_len = (size_t)(sp2 - after_mode);
    const char *stage_p = sp2 + 1;
    size_t stage_len = rest_len - oid_len - 1u;

    if (mode_len != 6u) {
        return atlas_err_set(err, ATLAS_ERR_GIT, "git ls-files mode is %zu bytes, expected 6",
                             mode_len);
    }
    for (size_t i = 0; i < mode_len; i++) {
        if (tok[i] < '0' || tok[i] > '7') {
            return atlas_err_set(err, ATLAS_ERR_GIT, "git ls-files mode is not octal");
        }
    }
    if (!atlas_git_is_hex_oid(after_mode, oid_len)) {
        return atlas_err_set(err, ATLAS_ERR_GIT, "git ls-files object id is not a hex oid");
    }
    int64_t stage = 0;
    if (!atlas_parse_i64(stage_p, stage_len, &stage) || stage < 0 || stage > 3) {
        return atlas_err_set(err, ATLAS_ERR_GIT, "git ls-files stage is not in 0..3");
    }

    size_t path_len = len - head_len - 1u;
    if (path_len == 0) {
        return atlas_err_set(err, ATLAS_ERR_GIT, "git ls-files record has an empty path");
    }

    char mode[7];
    char oid[ATLAS_OID_HEX_MAX_INCL];
    memcpy(mode, tok, 6u);
    mode[6] = '\0';
    memcpy(oid, after_mode, oid_len);
    oid[oid_len] = '\0';

    atlas_git_index_entry e;
    e.mode = mode;
    e.oid = oid;
    e.stage = (int)stage;
    e.path = tab + 1;
    e.path_len = path_len;
    p->entries++;
    if (p->cb == NULL) {
        return ATLAS_OK;
    }
    return p->cb(&e, p->ud, err);
}

/* --- log ---------------------------------------------------------------- */

const char *atlas_git_change_type_name(char kind) {
    switch (kind) {
    case 'A': return "add";
    case 'M': return "modify";
    case 'D': return "delete";
    case 'R': return "rename";
    case 'C': return "copy";
    case 'T': return "typechange";
    case 'U': return "unmerged";
    default: return "unknown";
    }
}

void atlas_log_parser_init(atlas_log_parser *p, atlas_git_commit_cb commit_cb,
                           atlas_git_change_cb change_cb, void *ud) {
    memset(p, 0, sizeof(*p));
    atlas_buf_init(&p->oid);
    atlas_buf_init(&p->parents);
    atlas_buf_init(&p->author_name);
    atlas_buf_init(&p->author_email);
    atlas_buf_init(&p->body);
    atlas_buf_init(&p->subject);
    atlas_buf_init(&p->path1);
    p->commit_cb = commit_cb;
    p->change_cb = change_cb;
    p->ud = ud;
}

void atlas_log_parser_free(atlas_log_parser *p) {
    atlas_buf_free(&p->oid);
    atlas_buf_free(&p->parents);
    atlas_buf_free(&p->author_name);
    atlas_buf_free(&p->author_email);
    atlas_buf_free(&p->body);
    atlas_buf_free(&p->subject);
    atlas_buf_free(&p->path1);
}

static void fill_commit(const atlas_log_parser *p, atlas_git_commit *c) {
    c->oid = atlas_buf_cstr(&p->oid);
    c->parents = atlas_buf_cstr(&p->parents);
    c->parent_count = p->parent_count;
    c->author_name = atlas_buf_cstr(&p->author_name);
    c->author_email = atlas_buf_cstr(&p->author_email);
    c->author_time = p->author_time;
    c->commit_time = p->commit_time;
    c->subject = atlas_buf_cstr(&p->subject);
    c->body = atlas_buf_cstr(&p->body);
    c->body_len = p->body.len;
}

/* Splits the commit header record. Field order is fixed and every field except
 * the trailing raw message is validated, so a message containing the field
 * separator cannot silently shift the parse. */
static atlas_status parse_commit_header(atlas_log_parser *p, const char *tok, size_t len,
                                        atlas_err *err) {
    /* tok[0] is the sentinel. */
    const char *cur = tok + 1;
    size_t remaining = len - 1u;
    const char *fields[6];
    size_t flens[6];
    for (int i = 0; i < 6; i++) {
        const char *fs = memchr(cur, ATLAS_LOG_FS, remaining);
        if (fs == NULL) {
            return atlas_err_set(err, ATLAS_ERR_GIT,
                                 "git log commit record is missing field %d of 7", i + 1);
        }
        fields[i] = cur;
        flens[i] = (size_t)(fs - cur);
        remaining -= flens[i] + 1u;
        cur = fs + 1;
    }
    const char *raw_msg = cur;
    size_t raw_msg_len = remaining;

    if (!atlas_git_is_hex_oid(fields[0], flens[0])) {
        return atlas_err_set(err, ATLAS_ERR_GIT, "git log commit record has a malformed object id");
    }
    int64_t at = 0;
    int64_t ct = 0;
    if (!atlas_parse_i64(fields[4], flens[4], &at) ||
        !atlas_parse_i64(fields[5], flens[5], &ct)) {
        return atlas_err_set(err, ATLAS_ERR_GIT,
                             "git log commit record has a malformed timestamp");
    }

    atlas_status st = atlas_buf_set(&p->oid, fields[0], flens[0], err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&p->parents, fields[1], flens[1], err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&p->author_name, fields[2], flens[2], err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&p->author_email, fields[3], flens[3], err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&p->body, raw_msg, raw_msg_len, err);
    }
    if (st != ATLAS_OK) {
        return st;
    }
    p->author_time = at;
    p->commit_time = ct;

    /* Parent count from the space-separated parent list. */
    p->parent_count = 0;
    bool in_tok = false;
    for (size_t i = 0; i < flens[1]; i++) {
        if (fields[1][i] == ' ') {
            in_tok = false;
        } else if (!in_tok) {
            in_tok = true;
            p->parent_count++;
        }
    }

    /* Subject is the first line of the raw message. */
    const char *nl = memchr(raw_msg, '\n', raw_msg_len);
    size_t subj_len = (nl != NULL) ? (size_t)(nl - raw_msg) : raw_msg_len;
    st = atlas_buf_set(&p->subject, raw_msg, subj_len, err);
    if (st != ATLAS_OK) {
        return st;
    }

    p->have_commit = true;
    p->commits_seen++;
    if (p->commit_cb != NULL) {
        atlas_git_commit c;
        fill_commit(p, &c);
        return p->commit_cb(&c, p->ud, err);
    }
    return ATLAS_OK;
}

/* A name-status code: one letter, optionally followed by a similarity score. */
static bool classify_status(const char *tok, size_t len, char *kind_out, int *score_out,
                            bool *score_known_out) {
    if (len == 0 || len > 4u) {
        return false;
    }
    char k = tok[0];
    if (strchr("ACDMRTUXB", k) == NULL) {
        return false;
    }
    int score = 0;
    bool have_score = false;
    if (len > 1u) {
        int64_t v = 0;
        if (!atlas_parse_i64(tok + 1, len - 1u, &v) || v < 0 || v > 100) {
            return false;
        }
        score = (int)v;
        have_score = true;
    }
    *kind_out = k;
    *score_out = score;
    *score_known_out = have_score;
    return true;
}

static atlas_status emit_change(atlas_log_parser *p, const char *path, size_t path_len,
                               atlas_err *err) {
    char kind = p->status[0];
    int score = 0;
    bool score_known = false;
    (void)classify_status(p->status, strlen(p->status), &kind, &score, &score_known);

    atlas_git_change ch;
    memset(&ch, 0, sizeof(ch));
    ch.raw_status = p->status;
    ch.kind = kind;
    ch.score = score;
    ch.score_known = score_known;
    if (kind == 'R' || kind == 'C') {
        ch.old_path = p->path1.data;
        ch.old_path_len = p->path1.len;
    }
    ch.path = path;
    ch.path_len = path_len;

    p->changes_seen++;
    if (p->change_cb == NULL) {
        return ATLAS_OK;
    }
    atlas_git_commit c;
    fill_commit(p, &c);
    return p->change_cb(&c, &ch, p->ud, err);
}

atlas_status atlas_log_token(const char *tok, size_t len, void *ud, atlas_err *err) {
    atlas_log_parser *p = (atlas_log_parser *)ud;

    /* Path operands are taken verbatim: a filename may legitimately start with a
     * newline or look exactly like a status code. */
    if (p->paths_needed > 0) {
        if (len == 0) {
            return atlas_err_set(err, ATLAS_ERR_GIT, "git log name-status path is empty");
        }
        if (p->paths_needed == 2) {
            atlas_status st = atlas_buf_set(&p->path1, tok, len, err);
            if (st != ATLAS_OK) {
                return st;
            }
            p->paths_needed = 1;
            return ATLAS_OK;
        }
        p->paths_needed = 0;
        return emit_change(p, tok, len, err);
    }

    /* git separates the format output from the diff section with a newline,
     * which lands at the start of the following record. */
    size_t off = 0;
    while (off < len && tok[off] == '\n') {
        off++;
    }
    const char *body = tok + off;
    size_t blen = len - off;
    if (blen == 0) {
        return ATLAS_OK; /* padding between sections */
    }

    if (body[0] == ATLAS_LOG_SENTINEL) {
        return parse_commit_header(p, body, blen, err);
    }

    char kind = 0;
    int score = 0;
    bool score_known = false;
    if (!classify_status(body, blen, &kind, &score, &score_known)) {
        return atlas_err_set(err, ATLAS_ERR_GIT,
                             "unrecognised git log record (%zu bytes, first byte 0x%02x)", blen,
                             (unsigned)(unsigned char)body[0]);
    }
    if (!p->have_commit) {
        return atlas_err_set(err, ATLAS_ERR_GIT,
                             "git log name-status entry appeared before any commit");
    }
    if (blen >= sizeof(p->status)) {
        return atlas_err_set(err, ATLAS_ERR_GIT, "git log status code is too long");
    }
    memcpy(p->status, body, blen);
    p->status[blen] = '\0';
    p->paths_needed = (kind == 'R' || kind == 'C') ? 2 : 1;
    return ATLAS_OK;
}

atlas_status atlas_log_parser_finish(atlas_log_parser *p, atlas_err *err) {
    if (p->paths_needed > 0) {
        return atlas_err_set(err, ATLAS_ERR_GIT,
                             "git log output ended while status %s was awaiting %d path(s)",
                             p->status, p->paths_needed);
    }
    return ATLAS_OK;
}

/* --- status --porcelain=v2 ---------------------------------------------- */

/* Record shapes, all space-separated with the path as the trailing remainder:
 *   1 <XY> <sub> <mH> <mI> <mW> <hH> <hI> <path>
 *   2 <XY> <sub> <mH> <mI> <mW> <hH> <hI> <Xscore> <path>   (orig path follows)
 *   u <XY> <sub> <m1> <m2> <m3> <mW> <h1> <h2> <h3> <path>
 *   ? <path>      ! <path>      # <header> <value>
 * X is the staged state against HEAD, Y the unstaged state against the index.
 */

/* Splits off `count` space-separated fields, leaving the untouched remainder as
 * the path. Returns false when the record has too few fields, which is a parse
 * error rather than something to work around. */
static bool split_fields(const char *tok, size_t len, size_t count, const char **fields,
                         size_t *flens, const char **path, size_t *path_len) {
    size_t pos = 0;
    for (size_t i = 0; i < count; i++) {
        const char *sp = memchr(tok + pos, ' ', len - pos);
        if (sp == NULL) {
            return false;
        }
        fields[i] = tok + pos;
        flens[i] = (size_t)(sp - (tok + pos));
        pos += flens[i] + 1u;
    }
    if (pos >= len) {
        return false; /* an empty path is malformed */
    }
    *path = tok + pos;
    *path_len = len - pos;
    return true;
}

void atlas_status_parser_init(atlas_status_parser *p, atlas_git_worktree_state *out,
                              atlas_git_status_cb cb, void *ud) {
    memset(p, 0, sizeof(*p));
    atlas_buf_init(&p->pending_path);
    atlas_buf_init(&p->pending_head_oid);
    atlas_buf_init(&p->pending_index_oid);
    for (size_t i = 0; i < 3; i++) {
        atlas_buf_init(&p->pending_modes[i]);
    }
    p->out = out;
    p->cb = cb;
    p->ud = ud;
}

void atlas_status_parser_free(atlas_status_parser *p) {
    atlas_buf_free(&p->pending_path);
    atlas_buf_free(&p->pending_head_oid);
    atlas_buf_free(&p->pending_index_oid);
    for (size_t i = 0; i < 3; i++) {
        atlas_buf_free(&p->pending_modes[i]);
    }
}

static atlas_status emit_entry(atlas_status_parser *p, atlas_change_scope scope, char status,
                              const char *score_text, const void *path, size_t path_len,
                              const void *old_path, size_t old_path_len, const char *head_oid,
                              const char *index_oid, const char *mode_head, const char *mode_index,
                              const char *mode_worktree, bool is_directory, atlas_err *err) {
    p->entries++;
    if (p->cb == NULL) {
        return ATLAS_OK;
    }
    atlas_git_status_entry e;
    memset(&e, 0, sizeof(e));
    e.scope = scope;
    e.status = status;
    if (score_text != NULL && score_text[0] != '\0') {
        int64_t v = 0;
        /* The score is preceded by its kind letter, e.g. "R100". */
        if (atlas_parse_i64(score_text + 1, strlen(score_text) - 1u, &v) && v >= 0 && v <= 100) {
            e.score = (int)v;
            e.score_known = true;
        }
    }
    e.path = path;
    e.path_len = path_len;
    e.old_path = old_path;
    e.old_path_len = old_path_len;
    e.head_oid = head_oid != NULL ? head_oid : "";
    e.index_oid = index_oid != NULL ? index_oid : "";
    e.mode_head = mode_head != NULL ? mode_head : "";
    e.mode_index = mode_index != NULL ? mode_index : "";
    e.mode_worktree = mode_worktree != NULL ? mode_worktree : "";
    e.is_directory = is_directory;
    return p->cb(&e, p->ud, err);
}

/* Emits the staged and/or unstaged sides of one tracked entry. */
static atlas_status emit_tracked(atlas_status_parser *p, const char *xy, const char *score_text,
                                 const void *path, size_t path_len, const void *old_path,
                                 size_t old_path_len, const char *head_oid, const char *index_oid,
                                 const char *mode_head, const char *mode_index,
                                 const char *mode_worktree, atlas_err *err) {
    char x = xy[0];
    char y = xy[1];
    atlas_status st = ATLAS_OK;
    if (x != '.') {
        p->out->staged++;
        p->out->dirty = true;
        /* Only the side that actually renamed carries the original path. */
        bool pair = (x == 'R' || x == 'C');
        st = emit_entry(p, ATLAS_SCOPE_STAGED, x, pair ? score_text : NULL, path, path_len,
                        pair ? old_path : NULL, pair ? old_path_len : 0u, head_oid, index_oid,
                        mode_head, mode_index, mode_worktree, false, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    if (y != '.') {
        p->out->unstaged++;
        p->out->dirty = true;
        bool pair = (y == 'R' || y == 'C');
        st = emit_entry(p, ATLAS_SCOPE_UNSTAGED, y, pair ? score_text : NULL, path, path_len,
                        pair ? old_path : NULL, pair ? old_path_len : 0u, head_oid, index_oid,
                        mode_head, mode_index, mode_worktree, false, err);
    }
    return st;
}

atlas_status atlas_status_token(const char *tok, size_t len, void *ud, atlas_err *err) {
    atlas_status_parser *p = (atlas_status_parser *)ud;
    atlas_git_worktree_state *out = p->out;

    /* The original path of a renamed entry arrives as its own record and is taken
     * verbatim, because a filename may contain anything. */
    if (p->pending_rename) {
        p->pending_rename = false;
        if (len == 0) {
            return atlas_err_set(err, ATLAS_ERR_GIT, "git status rename has an empty origin path");
        }
        return emit_tracked(p, p->pending_xy, p->pending_score, p->pending_path.data,
                            p->pending_path.len, tok, len,
                            atlas_buf_cstr(&p->pending_head_oid),
                            atlas_buf_cstr(&p->pending_index_oid),
                            atlas_buf_cstr(&p->pending_modes[0]),
                            atlas_buf_cstr(&p->pending_modes[1]),
                            atlas_buf_cstr(&p->pending_modes[2]), err);
    }
    if (len == 0) {
        return ATLAS_OK;
    }

    if (tok[0] == '#') {
        static const char branch_oid[] = "# branch.oid ";
        static const char branch_head[] = "# branch.head ";
        if (len > sizeof(branch_oid) - 1u &&
            memcmp(tok, branch_oid, sizeof(branch_oid) - 1u) == 0) {
            const char *v = tok + sizeof(branch_oid) - 1u;
            size_t vlen = len - (sizeof(branch_oid) - 1u);
            if (vlen == strlen("(initial)") && memcmp(v, "(initial)", vlen) == 0) {
                out->unborn = true;
                out->oid[0] = '\0';
            } else if (atlas_git_is_hex_oid(v, vlen)) {
                memcpy(out->oid, v, vlen);
                out->oid[vlen] = '\0';
            } else {
                return atlas_err_set(err, ATLAS_ERR_GIT,
                                     "git status reported a malformed branch oid");
            }
            return ATLAS_OK;
        }
        if (len > sizeof(branch_head) - 1u &&
            memcmp(tok, branch_head, sizeof(branch_head) - 1u) == 0) {
            const char *v = tok + sizeof(branch_head) - 1u;
            size_t vlen = len - (sizeof(branch_head) - 1u);
            if (vlen == strlen("(detached)") && memcmp(v, "(detached)", vlen) == 0) {
                out->branch[0] = '\0';
            } else {
                if (vlen >= sizeof(out->branch)) {
                    return atlas_err_set(err, ATLAS_ERR_GIT, "branch name exceeds %zu bytes",
                                         sizeof(out->branch) - 1u);
                }
                memcpy(out->branch, v, vlen);
                out->branch[vlen] = '\0';
            }
            return ATLAS_OK;
        }
        return ATLAS_OK; /* other headers are informational */
    }

    const char *fields[10];
    size_t flens[10];
    const char *path = NULL;
    size_t path_len = 0;

    switch (tok[0]) {
    case '1': {
        if (!split_fields(tok, len, 8u, fields, flens, &path, &path_len)) {
            return atlas_err_set(err, ATLAS_ERR_GIT, "malformed git status entry");
        }
        if (flens[1] != 2u) {
            return atlas_err_set(err, ATLAS_ERR_GIT, "malformed git status XY field");
        }
        char xy[3] = {fields[1][0], fields[1][1], '\0'};
        char mh[8] = {0};
        char mi[8] = {0};
        char mw[8] = {0};
        char hh[ATLAS_OID_HEX_MAX_INCL] = {0};
        char hi[ATLAS_OID_HEX_MAX_INCL] = {0};
        if (flens[3] >= sizeof(mh) || flens[4] >= sizeof(mi) || flens[5] >= sizeof(mw) ||
            flens[6] >= sizeof(hh) || flens[7] >= sizeof(hi)) {
            return atlas_err_set(err, ATLAS_ERR_GIT, "git status field is too long");
        }
        memcpy(mh, fields[3], flens[3]);
        memcpy(mi, fields[4], flens[4]);
        memcpy(mw, fields[5], flens[5]);
        memcpy(hh, fields[6], flens[6]);
        memcpy(hi, fields[7], flens[7]);
        return emit_tracked(p, xy, NULL, path, path_len, NULL, 0u, hh, hi, mh, mi, mw, err);
    }
    case '2': {
        if (!split_fields(tok, len, 9u, fields, flens, &path, &path_len)) {
            return atlas_err_set(err, ATLAS_ERR_GIT, "malformed git status rename entry");
        }
        if (flens[1] != 2u) {
            return atlas_err_set(err, ATLAS_ERR_GIT, "malformed git status XY field");
        }
        if (flens[8] == 0 || flens[8] >= sizeof(p->pending_score)) {
            return atlas_err_set(err, ATLAS_ERR_GIT, "malformed git status similarity field");
        }
        p->pending_xy[0] = fields[1][0];
        p->pending_xy[1] = fields[1][1];
        p->pending_xy[2] = '\0';
        memcpy(p->pending_score, fields[8], flens[8]);
        p->pending_score[flens[8]] = '\0';
        atlas_status st = atlas_buf_set(&p->pending_path, path, path_len, err);
        if (st == ATLAS_OK) {
            st = atlas_buf_set(&p->pending_modes[0], fields[3], flens[3], err);
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_set(&p->pending_modes[1], fields[4], flens[4], err);
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_set(&p->pending_modes[2], fields[5], flens[5], err);
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_set(&p->pending_head_oid, fields[6], flens[6], err);
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_set(&p->pending_index_oid, fields[7], flens[7], err);
        }
        if (st != ATLAS_OK) {
            return st;
        }
        p->pending_rename = true;
        return ATLAS_OK;
    }
    case 'u': {
        if (!split_fields(tok, len, 10u, fields, flens, &path, &path_len)) {
            return atlas_err_set(err, ATLAS_ERR_GIT, "malformed git status unmerged entry");
        }
        out->unmerged++;
        out->dirty = true;
        return emit_entry(p, ATLAS_SCOPE_UNMERGED, 'U', NULL, path, path_len, NULL, 0u, "", "", "",
                          "", "", false, err);
    }
    case '?': {
        if (len < 3u || tok[1] != ' ') {
            return atlas_err_set(err, ATLAS_ERR_GIT, "malformed git status untracked entry");
        }
        out->untracked++;
        out->dirty = true;
        /* git collapses a wholly untracked directory to a trailing slash. */
        bool is_dir = (tok[len - 1u] == '/');
        return emit_entry(p, ATLAS_SCOPE_UNTRACKED, '?', NULL, tok + 2, len - 2u, NULL, 0u, "", "",
                          "", "", "", is_dir, err);
    }
    case '!':
        return ATLAS_OK; /* ignored files are not dirt */
    default:
        return atlas_err_set(err, ATLAS_ERR_GIT,
                             "unrecognised git status record (first byte 0x%02x)",
                             (unsigned)(unsigned char)tok[0]);
    }
}

atlas_status atlas_status_parser_finish(atlas_status_parser *p, atlas_err *err) {
    if (p->pending_rename) {
        return atlas_err_set(err, ATLAS_ERR_GIT,
                             "git status output ended while a rename awaited its origin path");
    }
    return ATLAS_OK;
}

/* --- diff --numstat ----------------------------------------------------- */

void atlas_numstat_parser_init(atlas_numstat_parser *p, atlas_git_diff_cb cb, void *ud) {
    memset(p, 0, sizeof(*p));
    atlas_buf_init(&p->pending_old);
    p->cb = cb;
    p->ud = ud;
}

void atlas_numstat_parser_free(atlas_numstat_parser *p) {
    atlas_buf_free(&p->pending_old);
}

static atlas_status numstat_emit(atlas_numstat_parser *p, int64_t added, int64_t deleted,
                                 bool binary, const void *old_path, size_t old_len,
                                 const void *path, size_t path_len, atlas_err *err) {
    atlas_git_diff_entry e;
    memset(&e, 0, sizeof(e));
    e.added = added;
    e.deleted = deleted;
    e.binary = binary;
    e.old_path = old_path;
    e.old_path_len = old_len;
    e.path = path;
    e.path_len = path_len;
    p->entries++;
    if (p->cb == NULL) {
        return ATLAS_OK;
    }
    return p->cb(&e, p->ud, err);
}

atlas_status atlas_numstat_token(const char *tok, size_t len, void *ud, atlas_err *err) {
    atlas_numstat_parser *p = (atlas_numstat_parser *)ud;

    if (p->pending_paths > 0) {
        if (p->pending_paths == 2) {
            atlas_status st = atlas_buf_set(&p->pending_old, tok, len, err);
            if (st != ATLAS_OK) {
                return st;
            }
            p->pending_paths = 1;
            return ATLAS_OK;
        }
        p->pending_paths = 0;
        return numstat_emit(p, p->pending_added, p->pending_deleted, p->pending_binary,
                            p->pending_old.data, p->pending_old.len, tok, len, err);
    }

    if (len == 0) {
        return ATLAS_OK;
    }
    const char *t1 = memchr(tok, '\t', len);
    if (t1 == NULL) {
        return atlas_err_set(err, ATLAS_ERR_GIT, "git numstat record has no tab separator");
    }
    size_t add_len = (size_t)(t1 - tok);
    const char *after = t1 + 1;
    size_t after_len = len - add_len - 1u;
    const char *t2 = memchr(after, '\t', after_len);
    if (t2 == NULL) {
        return atlas_err_set(err, ATLAS_ERR_GIT, "git numstat record has one tab, expected two");
    }
    size_t del_len = (size_t)(t2 - after);
    const char *path = t2 + 1;
    size_t path_len = after_len - del_len - 1u;

    bool binary = (add_len == 1u && tok[0] == '-') && (del_len == 1u && after[0] == '-');
    int64_t added = -1;
    int64_t deleted = -1;
    if (!binary) {
        if (!atlas_parse_i64(tok, add_len, &added) ||
            !atlas_parse_i64(after, del_len, &deleted)) {
            return atlas_err_set(err, ATLAS_ERR_GIT, "git numstat counts are not numbers");
        }
    }

    if (path_len == 0) {
        /* Rename form: the two paths arrive as the next two records. */
        p->pending_added = added;
        p->pending_deleted = deleted;
        p->pending_binary = binary;
        p->pending_paths = 2;
        atlas_buf_reset(&p->pending_old);
        return ATLAS_OK;
    }
    return numstat_emit(p, added, deleted, binary, NULL, 0, path, path_len, err);
}

atlas_status atlas_numstat_parser_finish(atlas_numstat_parser *p, atlas_err *err) {
    if (p->pending_paths > 0) {
        return atlas_err_set(err, ATLAS_ERR_GIT,
                             "git numstat output ended awaiting %d rename path(s)",
                             p->pending_paths);
    }
    return ATLAS_OK;
}

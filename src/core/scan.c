/* Atlas - repository scanner.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Read-only with respect to the target repository. The whole scan runs inside
 * one database transaction, so a failure leaves no partial scan behind.
 */
#include "atlas/scan.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/pathrep.h"
#include "atlas/sha256.h"

#define ATLAS_HASH_CHUNK (64u * 1024u)
#define COMPILE_DB_NAME "compile_commands.json"

void atlas_scan_opts_init(atlas_scan_opts *o) {
    memset(o, 0, sizeof(*o));
    o->max_file_bytes = ATLAS_SCAN_DEFAULT_MAX_FILE_BYTES;
}

/* --- language detection -------------------------------------------------- */

const char *atlas_detect_language(const void *path_raw, size_t path_len) {
    static const struct {
        const char *ext;
        const char *lang;
    } BY_EXT[] = {
        {".c", "c"},        {".h", "c-header"},  {".i", "c"},          {".cc", "cpp"},
        {".cpp", "cpp"},    {".cxx", "cpp"},     {".hpp", "cpp-header"},
        {".hh", "cpp-header"}, {".hxx", "cpp-header"}, {".m", "objective-c"},
        {".mm", "objective-cpp"}, {".rs", "rust"}, {".go", "go"},      {".py", "python"},
        {".js", "javascript"}, {".mjs", "javascript"}, {".ts", "typescript"},
        {".tsx", "typescript"}, {".jsx", "javascript"}, {".java", "java"},
        {".kt", "kotlin"},  {".rb", "ruby"},     {".php", "php"},      {".cs", "csharp"},
        {".swift", "swift"}, {".sh", "shell"},   {".bash", "shell"},   {".zsh", "shell"},
        {".pl", "perl"},    {".lua", "lua"},     {".sql", "sql"},      {".md", "markdown"},
        {".markdown", "markdown"}, {".rst", "restructuredtext"}, {".txt", "text"},
        {".json", "json"},  {".yaml", "yaml"},   {".yml", "yaml"},     {".toml", "toml"},
        {".ini", "ini"},    {".cfg", "ini"},     {".xml", "xml"},      {".html", "html"},
        {".htm", "html"},   {".css", "css"},     {".scss", "scss"},    {".cmake", "cmake"},
        {".mk", "make"},    {".s", "assembly"},  {".S", "assembly"},   {".asm", "assembly"},
        {".zig", "zig"},    {".ex", "elixir"},   {".exs", "elixir"},   {".erl", "erlang"},
        {".hs", "haskell"}, {".scala", "scala"}, {".dart", "dart"},    {".proto", "protobuf"},
        {".patch", "diff"}, {".diff", "diff"},
    };
    static const struct {
        const char *name;
        const char *lang;
    } BY_NAME[] = {
        {"Makefile", "make"},        {"makefile", "make"},
        {"GNUmakefile", "make"},     {"CMakeLists.txt", "cmake"},
        {"Dockerfile", "dockerfile"}, {"LICENSE", "text"},
        {".gitignore", "gitignore"}, {".gitattributes", "gitattributes"},
    };

    const char *p = (const char *)path_raw;
    /* Basename: everything after the last '/'. */
    size_t base_start = 0;
    for (size_t i = 0; i < path_len; i++) {
        if (p[i] == '/') {
            base_start = i + 1u;
        }
    }
    const char *base = p + base_start;
    size_t base_len = path_len - base_start;
    if (base_len == 0) {
        return NULL;
    }

    for (size_t i = 0; i < sizeof(BY_NAME) / sizeof(BY_NAME[0]); i++) {
        size_t nl = strlen(BY_NAME[i].name);
        if (nl == base_len && memcmp(base, BY_NAME[i].name, nl) == 0) {
            return BY_NAME[i].lang;
        }
    }
    for (size_t i = 0; i < sizeof(BY_EXT) / sizeof(BY_EXT[0]); i++) {
        size_t el = strlen(BY_EXT[i].ext);
        if (base_len > el && memcmp(base + base_len - el, BY_EXT[i].ext, el) == 0) {
            return BY_EXT[i].lang;
        }
    }
    return NULL;
}

/* --- content hashing ----------------------------------------------------- */

/* Streams the file through SHA-256; the content is never held in memory. */
static atlas_status hash_fd(int fd, char *hex_out, uint64_t *size_out, atlas_err *err) {
    atlas_sha256 ctx;
    atlas_sha256_init(&ctx);
    unsigned char buf[ATLAS_HASH_CHUNK];
    uint64_t total = 0;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return atlas_err_set_errno(err, ATLAS_ERR_INTEGRITY, errno, "read failed while hashing");
        }
        if (n == 0) {
            break;
        }
        atlas_sha256_update(&ctx, buf, (size_t)n);
        total += (uint64_t)n;
    }
    unsigned char digest[ATLAS_SHA256_DIGEST_LEN];
    atlas_sha256_final(&ctx, digest);
    atlas_hex_encode(digest, sizeof(digest), hex_out);
    if (size_out != NULL) {
        *size_out = total;
    }
    return ATLAS_OK;
}

/* --- scan state ---------------------------------------------------------- */

typedef struct scan_ctx {
    atlas_db *db;
    atlas_git *g;
    int64_t repo_id;
    int64_t scan_id;
    const atlas_scan_opts *opts;
    atlas_scan_summary *sum;
    /* reusable buffers, so per-file work does not churn the allocator */
    atlas_buf path_text;
    atlas_buf link_target;
    atlas_buf old_path_text;
    bool compile_db_seen;
} scan_ctx;

static atlas_status record_evidence(scan_ctx *sc, atlas_evidence_kind kind, const char *git_oid,
                                    const void *path_raw, size_t path_len, const char *path_text,
                                    const char *commit_oid, const char *detail, atlas_err *err) {
    atlas_status st = atlas_db_evidence_insert(sc->db, sc->repo_id, kind, sc->scan_id, git_oid,
                                               path_raw, path_len, path_text, commit_oid, detail,
                                               err);
    if (st == ATLAS_OK) {
        sc->sum->evidence_created++;
    }
    return st;
}

static atlas_status note_compile_db(scan_ctx *sc, const void *path_raw, size_t path_len,
                                    atlas_err *err) {
    atlas_compile_db_record rec;
    memset(&rec, 0, sizeof(rec));

    atlas_buf text = ATLAS_BUF_INIT;
    atlas_status st = atlas_path_text_encode(path_raw, path_len, &text, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&text);
        return st;
    }

    char hex[ATLAS_SHA256_HEX_LEN + 1u];
    bool have_hash = false;
    atlas_path_open_result res = ATLAS_PATH_OPEN_MISSING;
    int fd = -1;
    struct stat sb;
    memset(&sb, 0, sizeof(sb));
    st = atlas_path_open_nofollow(atlas_git_root_fd(sc->g), (const char *)path_raw, path_len, &res,
                                 &fd, &sb, NULL, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&text);
        return st;
    }
    switch (res) {
    case ATLAS_PATH_OPEN_OK: {
        uint64_t size = 0;
        st = hash_fd(fd, hex, &size, err);
        (void)close(fd);
        if (st != ATLAS_OK) {
            atlas_buf_free(&text);
            return st;
        }
        have_hash = true;
        rec.is_regular_file = true;
        rec.size_bytes = (int64_t)size;
        rec.size_known = true;
        break;
    }
    case ATLAS_PATH_OPEN_SYMLINK: {
        /* Never followed: the link text itself is what Atlas records. */
        atlas_buf_reset(&sc->link_target);
        atlas_path_open_result lres = ATLAS_PATH_OPEN_MISSING;
        st = atlas_path_readlink_at(atlas_git_root_fd(sc->g), (const char *)path_raw, path_len,
                                    &sc->link_target, &lres, err);
        if (st != ATLAS_OK) {
            atlas_buf_free(&text);
            return st;
        }
        atlas_sha256_hex(sc->link_target.data, sc->link_target.len, hex);
        have_hash = true;
        rec.is_symlink = true;
        rec.size_bytes = (int64_t)sc->link_target.len;
        rec.size_known = true;
        break;
    }
    case ATLAS_PATH_OPEN_MISSING:
    case ATLAS_PATH_OPEN_UNSAFE:
    case ATLAS_PATH_OPEN_NOT_REGULAR:
    case ATLAS_PATH_OPEN_DENIED:
    default:
        atlas_buf_free(&text);
        return ATLAS_OK; /* nothing usable to record */
    }

    rec.path_raw = path_raw;
    rec.path_raw_len = path_len;
    rec.path_text = atlas_buf_cstr(&text);
    rec.content_hash = have_hash ? hex : NULL;

    /* A0 records the file's existence and identity; parsing waits for A2. */
    st = atlas_db_compile_db_upsert(sc->db, sc->repo_id, sc->scan_id, &rec, err);
    if (st == ATLAS_OK) {
        sc->sum->compile_db_found = true;
        if (rec.is_symlink) {
            sc->sum->compile_db_is_symlink = true;
        }
        st = record_evidence(sc, ATLAS_EV_SOURCE, NULL, path_raw, path_len,
                             atlas_buf_cstr(&text), NULL,
                             rec.is_symlink ? "compile_commands.json present as a symlink"
                                            : "compile_commands.json present as a regular file",
                             err);
    }
    atlas_buf_free(&text);
    return st;
}

static bool basename_is(const void *path_raw, size_t path_len, const char *name) {
    const char *p = (const char *)path_raw;
    size_t start = 0;
    for (size_t i = 0; i < path_len; i++) {
        if (p[i] == '/') {
            start = i + 1u;
        }
    }
    size_t nl = strlen(name);
    return (path_len - start) == nl && memcmp(p + start, name, nl) == 0;
}

/* --- per-file work ------------------------------------------------------- */

static atlas_status on_index_entry(const atlas_git_index_entry *e, void *ud, atlas_err *err) {
    scan_ctx *sc = (scan_ctx *)ud;

    /* Defence in depth: git should never hand us an absolute or dotted path. */
    atlas_status st = atlas_path_check_relative(e->path, e->path_len, err);
    if (st != ATLAS_OK) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "git reported an unusable tracked path: %s", atlas_err_msg(err));
    }

    atlas_buf_reset(&sc->path_text);
    st = atlas_path_text_encode(e->path, e->path_len, &sc->path_text, err);
    if (st != ATLAS_OK) {
        return st;
    }

    atlas_file_record rec;
    memset(&rec, 0, sizeof(rec));
    rec.path_raw = e->path;
    rec.path_raw_len = e->path_len;
    rec.path_text = atlas_buf_cstr(&sc->path_text);
    rec.path_is_utf8 = atlas_utf8_valid(e->path, e->path_len);
    rec.git_mode = e->mode;
    rec.git_index_oid = e->oid;
    rec.language = atlas_detect_language(e->path, e->path_len);
    rec.file_type = "missing";

    char hex[ATLAS_SHA256_HEX_LEN + 1u];
    bool gitlink = (strcmp(e->mode, "160000") == 0);
    bool mode_exec = (strcmp(e->mode, "100755") == 0);
    bool mode_link = (strcmp(e->mode, "120000") == 0);

    if (gitlink) {
        /* A submodule: its contents belong to another repository, which Atlas
         * would have to be told about separately. */
        rec.file_type = "other";
        rec.read_error = "submodule (gitlink); contents belong to another repository";
    } else {
        atlas_path_open_result res = ATLAS_PATH_OPEN_MISSING;
        int fd = -1;
        struct stat sb;
        memset(&sb, 0, sizeof(sb));
        st = atlas_path_open_nofollow(atlas_git_root_fd(sc->g), (const char *)e->path, e->path_len,
                                      &res, &fd, &sb, NULL, err);
        if (st != ATLAS_OK) {
            return st;
        }
        switch (res) {
        case ATLAS_PATH_OPEN_OK: {
            rec.file_type = "regular";
            rec.size_bytes = (int64_t)sb.st_size;
            rec.size_known = true;
            rec.is_executable = mode_exec || (sb.st_mode & S_IXUSR) != 0;
            uint64_t limit = sc->opts->max_file_bytes != 0 ? sc->opts->max_file_bytes
                                                           : ATLAS_SCAN_DEFAULT_MAX_FILE_BYTES;
            if ((uint64_t)sb.st_size > limit) {
                (void)close(fd);
                rec.read_error = "file exceeds the scan size limit; content not hashed";
                sc->sum->files_unreadable++;
            } else {
                uint64_t hashed = 0;
                st = hash_fd(fd, hex, &hashed, err);
                (void)close(fd);
                if (st != ATLAS_OK) {
                    /* A single unreadable file must not abort the whole scan. */
                    rec.read_error = "content could not be read";
                    atlas_err_init(err);
                    sc->sum->files_unreadable++;
                } else {
                    rec.content_hash = hex;
                    rec.content_hash_algo = "sha256";
                    rec.size_bytes = (int64_t)hashed;
                }
            }
            break;
        }
        case ATLAS_PATH_OPEN_SYMLINK: {
            /* For a tracked symlink the content *is* the link text. Atlas hashes
             * that text and never opens the target, so a link pointing outside
             * the repository cannot be read through. */
            atlas_buf_reset(&sc->link_target);
            atlas_path_open_result lres = ATLAS_PATH_OPEN_MISSING;
            st = atlas_path_readlink_at(atlas_git_root_fd(sc->g), (const char *)e->path,
                                        e->path_len, &sc->link_target, &lres, err);
            if (st != ATLAS_OK) {
                return st;
            }
            atlas_sha256_hex(sc->link_target.data, sc->link_target.len, hex);
            rec.file_type = "symlink";
            rec.is_symlink = true;
            rec.content_hash = hex;
            rec.content_hash_algo = "sha256";
            rec.size_bytes = (int64_t)sc->link_target.len;
            rec.size_known = true;
            rec.is_executable = false;
            break;
        }
        case ATLAS_PATH_OPEN_UNSAFE:
            rec.file_type = "other";
            rec.unsafe_path = true;
            rec.read_error = "refused: a path component is a symlink";
            sc->sum->files_unsafe++;
            break;
        case ATLAS_PATH_OPEN_NOT_REGULAR:
            rec.file_type = "other";
            rec.read_error = "not a regular file or symlink in the working tree";
            sc->sum->files_unreadable++;
            break;
        case ATLAS_PATH_OPEN_MISSING:
            rec.file_type = "missing";
            rec.read_error = "tracked but not present in the working tree";
            break;
        case ATLAS_PATH_OPEN_DENIED:
        default:
            rec.file_type = "other";
            rec.read_error = "cannot be opened";
            sc->sum->files_unreadable++;
            break;
        }
        if (mode_link && !rec.is_symlink && rec.read_error == NULL) {
            rec.read_error = "recorded as a symlink in the index but not in the working tree";
        }
    }

    atlas_upsert_kind kind = ATLAS_UPSERT_UNCHANGED;
    st = atlas_db_file_upsert(sc->db, sc->repo_id, sc->scan_id, &rec, &kind, err);
    if (st != ATLAS_OK) {
        return st;
    }
    sc->sum->files_total++;
    switch (kind) {
    case ATLAS_UPSERT_ADDED: sc->sum->files_added++; break;
    case ATLAS_UPSERT_MODIFIED: sc->sum->files_modified++; break;
    case ATLAS_UPSERT_UNCHANGED: sc->sum->files_unchanged++; break;
    default: break;
    }

    /* Evidence is recorded when a fact is new or has changed. An unchanged file
     * adds nothing, which is what makes repeated scans idempotent. */
    if (kind != ATLAS_UPSERT_UNCHANGED) {
        st = record_evidence(sc, ATLAS_EV_SOURCE, e->oid, e->path, e->path_len,
                             atlas_buf_cstr(&sc->path_text), NULL,
                             kind == ATLAS_UPSERT_ADDED ? "tracked file first seen by this scan"
                                                        : "tracked file changed since last scan",
                             err);
        if (st != ATLAS_OK) {
            return st;
        }
    }

    if (!sc->compile_db_seen && basename_is(e->path, e->path_len, COMPILE_DB_NAME)) {
        sc->compile_db_seen = true;
        st = note_compile_db(sc, e->path, e->path_len, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    return ATLAS_OK;
}

/* --- history ingestion --------------------------------------------------- */

typedef struct history_ctx {
    scan_ctx *sc;
    int64_t commit_id;
    bool commit_is_new;
    atlas_buf path_text;
    atlas_buf old_path_text;
} history_ctx;

static atlas_status on_commit(const atlas_git_commit *c, void *ud, atlas_err *err) {
    history_ctx *hc = (history_ctx *)ud;
    scan_ctx *sc = hc->sc;

    atlas_commit_record rec;
    memset(&rec, 0, sizeof(rec));
    rec.oid = c->oid;
    rec.parents = c->parents;
    rec.parent_count = c->parent_count;
    rec.author_name = c->author_name;
    rec.author_email = c->author_email;
    rec.author_time = c->author_time;
    rec.commit_time = c->commit_time;
    rec.subject = c->subject;
    rec.body = c->body;
    rec.body_len = c->body_len;

    bool inserted = false;
    atlas_status st = atlas_db_commit_upsert(sc->db, sc->repo_id, sc->scan_id, &rec, &hc->commit_id,
                                             &inserted, err);
    if (st != ATLAS_OK) {
        return st;
    }
    hc->commit_is_new = inserted;
    sc->sum->commits_seen++;
    if (inserted) {
        sc->sum->commits_ingested++;
        st = record_evidence(sc, ATLAS_EV_GIT, c->oid, NULL, 0, NULL, c->oid,
                             "commit metadata read from git log", err);
    }
    return st;
}

static atlas_status on_change(const atlas_git_commit *c, const atlas_git_change *ch, void *ud,
                              atlas_err *err) {
    history_ctx *hc = (history_ctx *)ud;
    scan_ctx *sc = hc->sc;
    (void)c;

    /* Changes belonging to a commit Atlas already has are already recorded;
     * re-inserting them would duplicate rows on every scan. */
    if (!hc->commit_is_new || hc->commit_id == 0) {
        return ATLAS_OK;
    }

    atlas_buf_reset(&hc->path_text);
    atlas_status st = atlas_path_text_encode(ch->path, ch->path_len, &hc->path_text, err);
    if (st != ATLAS_OK) {
        return st;
    }
    bool have_old = (ch->old_path != NULL && ch->old_path_len > 0);
    if (have_old) {
        atlas_buf_reset(&hc->old_path_text);
        st = atlas_path_text_encode(ch->old_path, ch->old_path_len, &hc->old_path_text, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }

    atlas_change_record rec;
    memset(&rec, 0, sizeof(rec));
    rec.change_type = atlas_git_change_type_name(ch->kind);
    rec.score = ch->score;
    rec.score_known = ch->score_known;
    rec.path_raw = ch->path;
    rec.path_raw_len = ch->path_len;
    rec.path_text = atlas_buf_cstr(&hc->path_text);
    rec.old_path_raw = have_old ? ch->old_path : NULL;
    rec.old_path_raw_len = have_old ? ch->old_path_len : 0u;
    rec.old_path_text = have_old ? atlas_buf_cstr(&hc->old_path_text) : NULL;
    rec.raw_status = ch->raw_status;

    st = atlas_db_change_insert(sc->db, sc->repo_id, hc->commit_id, &rec, err);
    if (st == ATLAS_OK) {
        sc->sum->changes_ingested++;
    }
    return st;
}

/* --- driver -------------------------------------------------------------- */

static atlas_status scan_untracked_compile_db(scan_ctx *sc, atlas_err *err) {
    /* compile_commands.json is usually generated and ignored by git, so it is
     * looked for directly as well as among the tracked files. */
    if (sc->compile_db_seen) {
        return ATLAS_OK;
    }
    struct stat sb;
    if (fstatat(atlas_git_root_fd(sc->g), COMPILE_DB_NAME, &sb, AT_SYMLINK_NOFOLLOW) != 0) {
        return ATLAS_OK;
    }
    sc->compile_db_seen = true;
    return note_compile_db(sc, COMPILE_DB_NAME, strlen(COMPILE_DB_NAME), err);
}

atlas_status atlas_scan_run(atlas_db *db, atlas_git *g, int64_t repo_id,
                            const atlas_scan_opts *opts, atlas_scan_summary *summary,
                            atlas_err *err) {
    atlas_scan_opts defaults;
    atlas_scan_opts_init(&defaults);
    if (opts == NULL) {
        opts = &defaults;
    }
    memset(summary, 0, sizeof(*summary));

    if (opts->timeout_ms > 0) {
        atlas_git_set_timeout_ms(g, opts->timeout_ms);
    }

    atlas_git_head head;
    atlas_status st = atlas_git_read_head(g, &head, err);
    if (st != ATLAS_OK) {
        return st;
    }
    atlas_git_worktree_state wt;
    st = atlas_git_read_worktree_state(g, &wt, err);
    if (st != ATLAS_OK) {
        return st;
    }

    atlas_scan_state state;
    memset(&state, 0, sizeof(state));
    state.head_oid = head.oid;
    state.head_state = head.state;
    state.branch = head.branch;
    state.object_format = atlas_git_object_format(g);
    state.dirty = wt.dirty;
    state.dirty_staged = wt.staged;
    state.dirty_unstaged = wt.unstaged;
    state.dirty_untracked = wt.untracked;
    state.dirty_unmerged = wt.unmerged;

    (void)snprintf(summary->head_oid, sizeof(summary->head_oid), "%s", head.oid);
    (void)snprintf(summary->head_state, sizeof(summary->head_state), "%s", head.state);
    (void)snprintf(summary->branch, sizeof(summary->branch), "%s", head.branch);
    summary->dirty = wt.dirty;
    summary->history_skipped = opts->skip_history;

    scan_ctx sc;
    memset(&sc, 0, sizeof(sc));
    sc.db = db;
    sc.g = g;
    sc.repo_id = repo_id;
    sc.opts = opts;
    sc.sum = summary;
    atlas_buf_init(&sc.path_text);
    atlas_buf_init(&sc.link_target);
    atlas_buf_init(&sc.old_path_text);

    history_ctx hc;
    memset(&hc, 0, sizeof(hc));
    hc.sc = &sc;
    atlas_buf_init(&hc.path_text);
    atlas_buf_init(&hc.old_path_text);

    st = atlas_db_begin(db, err);
    if (st != ATLAS_OK) {
        goto cleanup;
    }

    st = atlas_db_scan_begin(db, repo_id, &state, &sc.scan_id, err);
    if (st != ATLAS_OK) {
        goto rollback;
    }
    summary->scan_id = sc.scan_id;

    st = atlas_git_ls_files(g, on_index_entry, &sc, err);
    if (st != ATLAS_OK) {
        goto rollback;
    }

    st = atlas_db_files_mark_deleted(db, repo_id, sc.scan_id, &summary->files_deleted, err);
    if (st != ATLAS_OK) {
        goto rollback;
    }

    if (!opts->skip_history && strcmp(head.state, "unborn") != 0) {
        st = atlas_git_log(g, NULL, 0, opts->max_commits, on_commit, on_change, &hc, err);
        if (st != ATLAS_OK) {
            goto rollback;
        }
    }

    st = scan_untracked_compile_db(&sc, err);
    if (st != ATLAS_OK) {
        goto rollback;
    }

    st = atlas_db_scan_finish(db, repo_id, sc.scan_id, "ok", NULL, summary->files_total,
                              summary->files_added, summary->files_modified, summary->files_deleted,
                              summary->files_unchanged, summary->files_unreadable,
                              summary->commits_ingested, err);
    if (st != ATLAS_OK) {
        goto rollback;
    }
    st = atlas_db_repo_apply_scan(db, repo_id, sc.scan_id, &state, err);
    if (st != ATLAS_OK) {
        goto rollback;
    }

    st = atlas_db_commit(db, err);
    if (st != ATLAS_OK) {
        goto rollback;
    }

    /* The search index is derived data: a failure here is reported but leaves the
     * committed facts intact. */
    st = atlas_db_fts_rebuild(db, err);
    goto cleanup;

rollback:
    atlas_db_rollback(db);

cleanup:
    atlas_buf_free(&sc.path_text);
    atlas_buf_free(&sc.link_target);
    atlas_buf_free(&sc.old_path_text);
    atlas_buf_free(&hc.path_text);
    atlas_buf_free(&hc.old_path_text);
    return st;
}

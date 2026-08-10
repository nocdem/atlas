/* Atlas - the structural indexing pass.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A stage of the A1 reconciliation pass, not a second pipeline, and it inherits
 * A1's shape for A1's reasons:
 *
 *   select — one query, no I/O at all
 *   parse  — the worker pool, no transaction open, no database handle touched
 *   apply  — bounded transactions, no file read inside one
 *   resolve— deterministic, after everything is applied
 *
 * The selection query is the whole incremental story. It compares
 * `files.content_hash` — which A1 has already established — against the hash the
 * stored graph facts were extracted from. Not "was this file hashed by this
 * pass": a full content-verifying pass rehashes every byte and finds the same
 * hash, so an unchanged repository still selects nothing. Keying off the pass's
 * own activity would make the five-minute periodic full pass reparse the world
 * every five minutes, which is the one thing an incremental indexer must not do.
 *
 * Deletion is explicit here rather than a foreign key. `files` rows are
 * tombstoned rather than removed, so a cascade from `files` would fire only on
 * `repo remove` — the one case it is not needed for. A rename arrives as a
 * tombstone plus an addition, and therefore as a removal plus a parse, so
 * nothing has to recognise a rename as such.
 */
#define _GNU_SOURCE 1

#include "atlas/code.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/pathrep.h"
#include "atlas/safetext.h"
#include "atlas/sha256.h"

#define COMPILE_DB_NAME "compile_commands.json"
/* Bytes of a file's head used for content-based role classification. A
 * generated-file marker further in than this is a coincidence, not a header. */
#define ROLE_PREFIX_BYTES 4096u

void atlas_code_pass_opts_init(atlas_code_pass_opts *o) {
    memset(o, 0, sizeof(*o));
    o->root_fd = -1;
    o->max_files = ATLAS_CODE_MAX_PARSE_FILES_PER_PASS;
}

void atlas_code_pass_summary_init(atlas_code_pass_summary *s) {
    memset(s, 0, sizeof(*s));
}

static int64_t monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* --- the work list -----------------------------------------------------------
 *
 * One flat array with the path bytes in a single arena, so the table stays an
 * array of fixed-size records: the worker pool indexes it by job index, and each
 * job writes only its own slot. Exactly the shape A1's hash table has, for
 * exactly the same reason. */

typedef struct todo_item {
    size_t path_off;
    uint32_t path_len;
    int64_t file_id;
    int64_t code_file_id;
    char content_hash[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_code_language lang;

    /* Filled by the worker, read by the writer. Nothing else touches them. */
    atlas_code_parse parse;
    atlas_code_roles roles;
    bool read_failed;
    char read_error[128];
} todo_item;

typedef struct todo_table {
    atlas_buf arena;
    todo_item *items;
    size_t count;
    size_t cap;
} todo_table;

static void todo_init(todo_table *t) {
    memset(t, 0, sizeof(*t));
    atlas_buf_init(&t->arena);
}

static void todo_free(todo_table *t) {
    for (size_t i = 0; i < t->count; i++) {
        atlas_code_parse_free(&t->items[i].parse);
    }
    free(t->items);
    atlas_buf_free(&t->arena);
    memset(t, 0, sizeof(*t));
}

static const char *todo_path(const todo_table *t, const todo_item *it) {
    return t->arena.data + it->path_off;
}

typedef struct select_ctx {
    todo_table *table;
    int64_t limit;
    bool truncated;
} select_ctx;

static atlas_status todo_add(const atlas_code_todo_row *row, void *ud, atlas_err *err) {
    select_ctx *sc = (select_ctx *)ud;
    todo_table *t = sc->table;

    /* The SQL pre-filtered on extension with a case-insensitive LIKE; this is
     * the authority, and it is case-sensitive on purpose. `.C` is C++ by
     * convention and A3 does not guess at C++. */
    atlas_code_language lang = atlas_code_language_of(row->path_raw, row->path_raw_len);
    if (lang == ATLAS_CODE_LANG_NONE) {
        return ATLAS_OK;
    }
    if (sc->limit > 0 && (int64_t)t->count >= sc->limit) {
        sc->truncated = true;
        return ATLAS_OK;
    }
    if (t->count == t->cap) {
        size_t next = t->cap == 0 ? 256u : t->cap * 2u;
        todo_item *grown = realloc(t->items, next * sizeof(*grown));
        if (grown == NULL) {
            return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                                 "out of memory selecting files for structural indexing");
        }
        t->items = grown;
        t->cap = next;
    }
    todo_item *it = &t->items[t->count];
    memset(it, 0, sizeof(*it));
    atlas_code_parse_init(&it->parse);
    it->path_off = t->arena.len;
    it->path_len = (uint32_t)row->path_raw_len;
    it->file_id = row->file_id;
    it->code_file_id = row->code_file_id;
    it->lang = lang;
    if (row->content_hash != NULL) {
        (void)snprintf(it->content_hash, sizeof(it->content_hash), "%s", row->content_hash);
    }
    atlas_status st = atlas_buf_append(&t->arena, row->path_raw, row->path_raw_len, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_append_ch(&t->arena, '\0', err);
    }
    if (st != ATLAS_OK) {
        return st;
    }
    t->count++;
    return ATLAS_OK;
}

/* --- the parse job ------------------------------------------------------------
 *
 * Runs on a worker thread. It reads bytes through the same no-follow walker the
 * hash stage uses and extracts structure from them. It touches no database
 * handle and creates no process — the same two rules the hash jobs beside it
 * obey, and for the same reason: the single-writer model is provable only
 * because nothing else can write. */

typedef struct parse_ctx {
    todo_table *table;
    int root_fd;
} parse_ctx;

static void parse_job(size_t i, void *ud) {
    parse_ctx *pc = (parse_ctx *)ud;
    todo_item *it = &pc->table->items[i];
    const char *rel = todo_path(pc->table, it);

    atlas_err err;
    atlas_err_init(&err);
    atlas_path_open_result res = ATLAS_PATH_OPEN_MISSING;
    int fd = -1;
    struct stat sb;
    memset(&sb, 0, sizeof(sb));
    /* Never follows a symlink at any component. A repository that plants one
     * cannot make the structural indexer read a file outside it. */
    if (atlas_path_open_nofollow(pc->root_fd, rel, it->path_len, &res, &fd, &sb, NULL, &err) !=
        ATLAS_OK) {
        it->read_failed = true;
        (void)snprintf(it->read_error, sizeof(it->read_error), "%s", "the file could not be opened");
        return;
    }
    if (res != ATLAS_PATH_OPEN_OK) {
        if (fd >= 0) {
            (void)close(fd);
        }
        /* A symlink's content is its link text, which is not source. Anything
         * else — missing, unsafe, not regular — is likewise not something to
         * extract structure from, and saying so beats recording an empty file. */
        it->read_failed = true;
        (void)snprintf(it->read_error, sizeof(it->read_error), "%s",
                       res == ATLAS_PATH_OPEN_SYMLINK
                           ? "a symlink's target is not read by the structural indexer"
                           : "the file is not a readable regular file");
        return;
    }

    atlas_buf content = ATLAS_BUF_INIT;
    bool over = false;
    for (;;) {
        char chunk[64u * 1024u];
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            it->read_failed = true;
            (void)snprintf(it->read_error, sizeof(it->read_error), "%s",
                           "the file could not be read");
            break;
        }
        if (n == 0) {
            break;
        }
        if (content.len + (size_t)n > ATLAS_CODE_MAX_FILE_BYTES) {
            over = true;
            break;
        }
        if (atlas_buf_append(&content, chunk, (size_t)n, &err) != ATLAS_OK) {
            it->read_failed = true;
            (void)snprintf(it->read_error, sizeof(it->read_error), "%s",
                           "out of memory reading the file");
            break;
        }
    }
    (void)close(fd);

    if (!it->read_failed) {
        atlas_err perr;
        atlas_err_init(&perr);
        if (atlas_code_extract(content.data, content.len, it->lang, &it->parse, &perr) !=
            ATLAS_OK) {
            it->read_failed = true;
            (void)snprintf(it->read_error, sizeof(it->read_error), "%s",
                           "the structural parse could not complete");
        } else if (over) {
            /* The ceiling was reached during the read rather than trusted from
             * an earlier stat, because a file can grow between the two. */
            it->parse.truncated = true;
            it->parse.truncated_reason = "the file exceeds the structural parse ceiling";
            if (it->parse.status == ATLAS_CODE_PARSE_OK) {
                it->parse.status = ATLAS_CODE_PARSE_PARTIAL;
            }
        }
        size_t prefix = content.len < ROLE_PREFIX_BYTES ? content.len : ROLE_PREFIX_BYTES;
        atlas_code_classify_roles(rel, it->path_len, content.data, prefix, &it->roles);
    } else {
        atlas_code_classify_roles(rel, it->path_len, NULL, 0, &it->roles);
    }
    atlas_buf_free(&content);
}

/* --- applying one file's result ------------------------------------------------ */

typedef struct apply_ctx {
    atlas_db *db;
    int64_t repo_id;
    int64_t generation;
    atlas_code_pass_summary *sum;
    atlas_buf path_text;
    atlas_buf name_text;
    /* Every *externally linked* symbol name this pass defined or removed, NUL
     * separated. Resolution uses it to re-resolve the edges elsewhere that
     * mention those names, which is what makes a header change cheap without
     * being wrong.
     *
     * Internal linkage is excluded, and that is a correctness rule wearing a
     * performance hat. A `static` definition is a candidate only inside its own
     * file, so changing one cannot change how an edge in any other file
     * resolves; the edges it *can* change all belong to the file just reparsed,
     * and those were rewritten unresolved and are swept by file. Including them
     * here would sweep every edge in the repository naming `helper` — thousands
     * of them in a real C project — to reach the handful that could differ. */
    atlas_buf touched_names;
    bool touched_overflow;
    /* Every `code_files` id this pass parsed or removed, as raw int64. Their
     * edges were rewritten unresolved, so resolution must settle them whether or
     * not anybody changed a name they mention. Bounded by the parse ceiling,
     * which is why it cannot overflow silently: past it the scope says `full`.
     *
     * `file_set_changed` is separate and narrower: a path appeared or left. Only
     * that can make a previously unresolvable *include* resolvable, and it is
     * the difference between an ordinary edit and a repository-wide sweep. */
    atlas_buf parsed_files;
    bool parsed_overflow;
    bool file_set_changed;
    int64_t in_batch;
} apply_ctx;

/* Records one file as needing its own edges settled. */
static atlas_status note_parsed_file(apply_ctx *ac, int64_t code_file_id, atlas_err *err) {
    if (code_file_id <= 0) {
        return ATLAS_OK;
    }
    if (ac->parsed_files.len / sizeof(int64_t) >= (size_t)ATLAS_CODE_MAX_PARSE_FILES_PER_PASS) {
        ac->parsed_overflow = true;
        return ATLAS_OK;
    }
    return atlas_buf_append(&ac->parsed_files, &code_file_id, sizeof(code_file_id), err);
}

static atlas_status batch_begin(apply_ctx *ac, atlas_err *err) {
    if (ac->in_batch == 0) {
        atlas_status st = atlas_db_begin(ac->db, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    ac->in_batch++;
    return ATLAS_OK;
}

static atlas_status batch_maybe_commit(apply_ctx *ac, bool force, atlas_err *err) {
    if (ac->in_batch == 0) {
        return ATLAS_OK;
    }
    if (!force && ac->in_batch < ATLAS_DB_BATCH_MAX) {
        return ATLAS_OK;
    }
    atlas_status st = atlas_db_commit(ac->db, err);
    ac->in_batch = 0;
    return st;
}

static atlas_status note_touched(apply_ctx *ac, const char *name, size_t len, atlas_err *err) {
    if (ac->touched_overflow || len == 0) {
        return ATLAS_OK;
    }
    if (ac->touched_names.len + len + 1u > (size_t)ATLAS_CODE_MAX_RESOLVE_NAMES * 64u) {
        /* Past the ceiling the set stops being useful and resolution falls back
         * to the whole repository — which is reported, not silent. */
        ac->touched_overflow = true;
        return ATLAS_OK;
    }
    atlas_status st = atlas_buf_append(&ac->touched_names, name, len, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_append_ch(&ac->touched_names, '\0', err);
    }
    return st;
}

/* Collects the names a file currently defines, before its rows are replaced, so
 * a definition that *disappears* invalidates the edges naming it just as one
 * that appears does. */
static atlas_status note_existing(const atlas_code_symbol_row *row, void *ud, atlas_err *err) {
    apply_ctx *ac = (apply_ctx *)ud;
    if (!row->is_definition || row->name_text == NULL) {
        return ATLAS_OK;
    }
    /* Same rule as the new-definition side: an internal definition is invisible
     * outside its own file, and that file is swept by id. */
    if (row->linkage != NULL && strcmp(row->linkage, "internal") == 0) {
        return ATLAS_OK;
    }
    /* The safe text form is what is stored; a name that needed escaping is not a
     * C identifier and will not match a call candidate anyway. */
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_status st = atlas_path_text_decode(row->name_text, strlen(row->name_text), &raw, err);
    if (st == ATLAS_OK) {
        st = note_touched(ac, raw.data, raw.len, err);
    }
    atlas_buf_free(&raw);
    return st;
}

static atlas_status remember_old_definitions(apply_ctx *ac, int64_t code_file_id, atlas_err *err) {
    if (code_file_id <= 0) {
        return ATLAS_OK;
    }
    int64_t n = 0;
    bool more = false;
    return atlas_db_code_symbols_in_file(ac->db, code_file_id, ATLAS_CODE_MAX_SYMBOLS_PER_FILE,
                                         note_existing, ac, &n, &more, err);
}

static atlas_status apply_file(apply_ctx *ac, const todo_table *t, todo_item *it,
                               atlas_err *err) {
    const char *rel = todo_path(t, it);
    atlas_buf_reset(&ac->path_text);
    atlas_status st = atlas_path_text_encode(rel, it->path_len, &ac->path_text, err);
    if (st != ATLAS_OK) {
        return st;
    }

    st = remember_old_definitions(ac, it->code_file_id, err);
    if (st != ATLAS_OK) {
        return st;
    }
    /* Before the rows go: the edges elsewhere that resolved to this file's
     * symbols point at ids that are about to stop existing. Unsettling them here
     * — by seeking the destinations that are about to vanish — is what lets the
     * pass skip the repository-wide dangling scan, which is a left join over
     * every relation and cannot be made cheap.
     *
     * The file node itself survives a reparse: `code_file_upsert` keeps the row
     * and its id, so an include resolved to this file is still correct. */
    if (it->code_file_id > 0) {
        int64_t unsettled = 0;
        st = atlas_db_code_relations_unsettle_for_file(ac->db, ac->repo_id, it->code_file_id, false,
                                                       ac->generation, &unsettled, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }

    atlas_code_file_record rec;
    memset(&rec, 0, sizeof(rec));
    rec.file_id = it->file_id;
    rec.path_raw = rel;
    rec.path_raw_len = it->path_len;
    rec.path_text = atlas_buf_cstr(&ac->path_text);
    rec.language = atlas_code_language_name(it->lang);
    rec.content_hash = it->content_hash[0] != '\0' ? it->content_hash : NULL;
    rec.generation = ac->generation;
    if (it->read_failed) {
        rec.parse_status = atlas_code_parse_status_name(ATLAS_CODE_PARSE_FAILED);
        rec.parse_detail = it->read_error;
        ac->sum->files_failed++;
    } else {
        rec.parse_status = atlas_code_parse_status_name(it->parse.status);
        rec.truncated = it->parse.truncated;
        rec.truncated_reason = it->parse.truncated_reason;
        rec.include_guard = it->parse.include_guard;
        rec.symbol_count = (int64_t)it->parse.symbol_count;
        rec.include_count = (int64_t)it->parse.include_count;
        rec.occurrence_count = (int64_t)it->parse.occurrence_count;
        rec.bytes = it->parse.bytes;
        rec.lines = it->parse.lines;
        if (it->parse.status == ATLAS_CODE_PARSE_PARTIAL) {
            ac->sum->files_partial++;
        } else if (it->parse.status == ATLAS_CODE_PARSE_FAILED) {
            ac->sum->files_failed++;
        }
    }

    int64_t code_file_id = 0;
    st = atlas_db_code_file_upsert(ac->db, ac->repo_id, &rec, &code_file_id, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (it->code_file_id == 0) {
        /* A path Atlas had no structural row for: the file set grew, and an
         * include somewhere that could not be placed may be placeable now. */
        ac->file_set_changed = true;
    }
    it->code_file_id = code_file_id;
    st = note_parsed_file(ac, code_file_id, err);
    if (st != ATLAS_OK) {
        return st;
    }
    /* Replace rather than accumulate. Everything this file owned goes, then the
     * new facts go in; there is no path on which a stale symbol survives a
     * reparse. */
    st = atlas_db_code_file_clear(ac->db, code_file_id, err);
    if (st != ATLAS_OK) {
        return st;
    }

    for (size_t i = 0; st == ATLAS_OK && i < it->roles.count; i++) {
        st = atlas_db_code_role_add(
            ac->db, code_file_id, atlas_code_role_name((atlas_code_role)it->roles.items[i].role),
            atlas_code_role_basis_name((atlas_code_role_basis)it->roles.items[i].basis),
            atlas_code_resolution_name((atlas_code_resolution)it->roles.items[i].resolution), err);
    }
    if (st != ATLAS_OK || it->read_failed) {
        return st;
    }

    /* Symbols first: occurrences and relations refer to their ids. The parse's
     * own indices map to database ids through this array, which is why the
     * extractor hands back indices rather than trying to invent identities. */
    int64_t *sym_ids = NULL;
    if (it->parse.symbol_count > 0) {
        sym_ids = calloc(it->parse.symbol_count, sizeof(*sym_ids));
        if (sym_ids == NULL) {
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory applying symbols");
        }
    }
    for (size_t i = 0; st == ATLAS_OK && i < it->parse.symbol_count; i++) {
        const atlas_code_symbol_item *s = &it->parse.symbols[i];
        const char *name = atlas_code_parse_name(&it->parse, s->name_off);
        atlas_buf_reset(&ac->name_text);
        /* A C identifier is ASCII in practice and not guaranteed to be. The safe
         * encoding is applied to the display form for the same reason it is
         * applied to a path, and the raw bytes stay the lookup key. */
        st = atlas_path_text_encode(name, s->name_len, &ac->name_text, err);
        if (st != ATLAS_OK) {
            break;
        }
        atlas_code_symbol_record sr;
        memset(&sr, 0, sizeof(sr));
        sr.name = name;
        sr.name_len = s->name_len;
        sr.name_text = atlas_buf_cstr(&ac->name_text);
        sr.kind = atlas_code_symbol_kind_name((atlas_code_symbol_kind)s->kind);
        sr.linkage = atlas_code_linkage_name((atlas_code_linkage)s->linkage);
        sr.resolution = atlas_code_resolution_name((atlas_code_resolution)s->resolution);
        sr.is_definition = s->is_definition;
        sr.is_declaration = s->is_declaration;
        sr.line = s->line;
        sr.col = s->col;
        sr.byte_offset = s->byte_offset;
        sr.end_line = s->end_line;
        sr.generation = ac->generation;
        st = atlas_db_code_symbol_insert(ac->db, ac->repo_id, code_file_id, &sr, &sym_ids[i], err);
        if (st != ATLAS_OK) {
            break;
        }
        ac->sum->symbols_written++;

        atlas_code_relation_record rr;
        memset(&rr, 0, sizeof(rr));
        rr.owner_file_id = code_file_id;
        rr.kind = atlas_code_rel_kind_name(s->is_definition
                                               ? ATLAS_CODE_REL_FILE_DEFINES_SYMBOL
                                               : ATLAS_CODE_REL_FILE_DECLARES_SYMBOL);
        rr.src_kind = "file";
        rr.src_id = code_file_id;
        rr.dst_kind = "symbol";
        rr.dst_id = sym_ids[i];
        rr.dst_name = name;
        rr.dst_name_len = s->name_len;
        rr.dst_name_text = atlas_buf_cstr(&ac->name_text);
        rr.resolution = sr.resolution;
        rr.provenance = atlas_code_provenance_name(ATLAS_CODE_PROV_SOURCE);
        rr.line = s->line;
        rr.col = s->col;
        rr.generation = ac->generation;
        int64_t rel_id = 0;
        st = atlas_db_code_relation_insert(ac->db, ac->repo_id, &rr, &rel_id, err);
        if (st == ATLAS_OK) {
            ac->sum->relations_written++;
        }
        if (st == ATLAS_OK && s->is_definition && s->linkage != ATLAS_CODE_LINK_INTERNAL) {
            st = note_touched(ac, name, s->name_len, err);
        }
        if (st == ATLAS_OK && s->is_declaration && !s->is_definition &&
            (s->kind == ATLAS_CODE_SYM_FUNCTION || s->kind == ATLAS_CODE_SYM_VARIABLE)) {
            /* A declaration that names something it does not define wants to
             * know where the definition is. Written unresolved; the resolver
             * settles it, with the same rules and the same classes a call gets. */
            atlas_code_relation_record dr;
            memset(&dr, 0, sizeof(dr));
            dr.owner_file_id = code_file_id;
            dr.kind = atlas_code_rel_kind_name(ATLAS_CODE_REL_SYMBOL_DEFINED_BY);
            dr.src_kind = "symbol";
            dr.src_id = sym_ids[i];
            dr.dst_kind = "unresolved";
            dr.dst_name = name;
            dr.dst_name_len = s->name_len;
            dr.dst_name_text = atlas_buf_cstr(&ac->name_text);
            dr.resolution = atlas_code_resolution_name(ATLAS_CODE_RES_UNRESOLVED);
            dr.provenance = atlas_code_provenance_name(ATLAS_CODE_PROV_SOURCE);
            dr.line = s->line;
            dr.col = s->col;
            dr.generation = ac->generation;
            int64_t did = 0;
            st = atlas_db_code_relation_insert(ac->db, ac->repo_id, &dr, &did, err);
            if (st == ATLAS_OK) {
                ac->sum->relations_written++;
            }
        }
    }

    /* Includes: the spelling is recorded whether or not anything resolves it.
     * "This file includes something called config.h that I cannot place" is a
     * fact worth having, and dropping it would be a silence. */
    for (size_t i = 0; st == ATLAS_OK && i < it->parse.include_count; i++) {
        const atlas_code_include_item *inc = &it->parse.includes[i];
        const char *spell = atlas_code_parse_name(&it->parse, inc->spelling_off);
        atlas_buf_reset(&ac->name_text);
        st = atlas_path_text_encode(spell, inc->spelling_len, &ac->name_text, err);
        if (st != ATLAS_OK) {
            break;
        }
        atlas_code_relation_record rr;
        memset(&rr, 0, sizeof(rr));
        rr.owner_file_id = code_file_id;
        rr.kind = atlas_code_rel_kind_name(ATLAS_CODE_REL_FILE_INCLUDES_FILE);
        rr.src_kind = "file";
        rr.src_id = code_file_id;
        rr.dst_kind = "unresolved";
        rr.dst_name = spell;
        rr.dst_name_len = inc->spelling_len;
        rr.dst_name_text = atlas_buf_cstr(&ac->name_text);
        rr.spelling_form = (inc->form == ATLAS_CODE_INCLUDE_ANGLE) ? "angle" : "quote";
        rr.resolution = atlas_code_resolution_name(ATLAS_CODE_RES_UNRESOLVED);
        rr.provenance = atlas_code_provenance_name(ATLAS_CODE_PROV_SOURCE);
        rr.line = inc->line;
        rr.col = inc->col;
        rr.generation = ac->generation;
        int64_t rel_id = 0;
        st = atlas_db_code_relation_insert(ac->db, ac->repo_id, &rr, &rel_id, err);
        if (st == ATLAS_OK) {
            ac->sum->relations_written++;
        }
    }

    /* Call candidates. The occurrence's existence is exact; what it refers to is
     * the resolver's problem, with weaker classes. */
    for (size_t i = 0; st == ATLAS_OK && i < it->parse.occurrence_count; i++) {
        const atlas_code_occurrence_item *oc = &it->parse.occurrences[i];
        const char *name = atlas_code_parse_name(&it->parse, oc->name_off);
        atlas_buf_reset(&ac->name_text);
        st = atlas_path_text_encode(name, oc->name_len, &ac->name_text, err);
        if (st != ATLAS_OK) {
            break;
        }
        int64_t enclosing = 0;
        if (oc->enclosing >= 0 && (size_t)oc->enclosing < it->parse.symbol_count &&
            sym_ids != NULL) {
            enclosing = sym_ids[oc->enclosing];
        }
        atlas_code_occurrence_record orr;
        memset(&orr, 0, sizeof(orr));
        orr.name = name;
        orr.name_len = oc->name_len;
        orr.name_text = atlas_buf_cstr(&ac->name_text);
        orr.resolution = atlas_code_resolution_name(ATLAS_CODE_RES_UNRESOLVED);
        orr.enclosing_id = enclosing;
        orr.line = oc->line;
        orr.col = oc->col;
        orr.byte_offset = oc->byte_offset;
        orr.generation = ac->generation;
        int64_t occ_id = 0;
        st = atlas_db_code_occurrence_insert(ac->db, ac->repo_id, code_file_id, &orr, &occ_id, err);
        if (st != ATLAS_OK) {
            break;
        }

        /* `symbol_contains_occurrence` is deliberately **not** materialised here.
         *
         * The containment fact is real and is stored — as
         * `code_occurrences.enclosing_id`, which is where the extractor put it,
         * which has a foreign key and an index, and which is the row every
         * consumer already reads. Writing an edge as well stored the same fact
         * twice: on the acceptance fixture that was 235 520 relation rows,
         * 38 % of the entire relation table, each carrying five index
         * insertions, and not one query in Atlas read a single one of them.
         * Caller-to-callee traversal does not need them either, because
         * `symbol_calls_symbol` already carries the enclosing symbol as its
         * source.
         *
         * The kind stays in the vocabulary and in the schema's CHECK. It is a
         * legitimate edge for a producer that has no occurrence table of its
         * own — a future importer, say — and removing it from the vocabulary
         * would make that a migration rather than an insert. What is removed is
         * the duplication, not the fact and not the ability to record it. */

        atlas_code_relation_record kr;
        memset(&kr, 0, sizeof(kr));
        kr.owner_file_id = code_file_id;
        kr.kind = atlas_code_rel_kind_name(ATLAS_CODE_REL_SYMBOL_CALLS_SYMBOL);
        kr.src_kind = enclosing > 0 ? "symbol" : "file";
        kr.src_id = enclosing > 0 ? enclosing : code_file_id;
        kr.dst_kind = "unresolved";
        kr.dst_name = name;
        kr.dst_name_len = oc->name_len;
        kr.dst_name_text = atlas_buf_cstr(&ac->name_text);
        kr.resolution = atlas_code_resolution_name(ATLAS_CODE_RES_UNRESOLVED);
        kr.provenance = atlas_code_provenance_name(ATLAS_CODE_PROV_SOURCE);
        kr.line = oc->line;
        kr.col = oc->col;
        kr.generation = ac->generation;
        int64_t kid = 0;
        st = atlas_db_code_relation_insert(ac->db, ac->repo_id, &kr, &kid, err);
        if (st == ATLAS_OK) {
            ac->sum->relations_written++;
        }
    }

    free(sym_ids);
    if (st == ATLAS_OK) {
        ac->sum->files_parsed++;
    }
    return st;
}

/* --- removals ------------------------------------------------------------------ */

typedef struct remove_ctx {
    apply_ctx *ac;
    int64_t *ids;
    size_t count;
    size_t cap;
} remove_ctx;

static atlas_status remove_note(const atlas_code_todo_row *row, void *ud, atlas_err *err) {
    remove_ctx *rc = (remove_ctx *)ud;
    if (rc->count == rc->cap) {
        size_t next = rc->cap == 0 ? 64u : rc->cap * 2u;
        int64_t *grown = realloc(rc->ids, next * sizeof(*grown));
        if (grown == NULL) {
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory collecting removals");
        }
        rc->ids = grown;
        rc->cap = next;
    }
    rc->ids[rc->count++] = row->code_file_id;
    return ATLAS_OK;
}

/* --- the compile database -------------------------------------------------------- */

/* Reads the compile database, if there is one, and replaces the recorded units
 * when its content changed.
 *
 * Nothing here executes anything. The file is opened through the same no-follow
 * walker every other read uses, bounded before it is parsed, and handed to
 * `atlas_code_compdb_parse`, which stores a hash of the `command` string and not
 * the string. */
static atlas_status ingest_compile_db(atlas_db *db, int64_t repo_id, int64_t generation,
                                      const atlas_code_pass_opts *opts, bool rebuild,
                                      const atlas_code_index_state *state,
                                      atlas_code_pass_summary *sum, atlas_err *err) {
    atlas_path_open_result res = ATLAS_PATH_OPEN_MISSING;
    int fd = -1;
    struct stat sb;
    memset(&sb, 0, sizeof(sb));
    atlas_err ignore;
    atlas_err_init(&ignore);
    if (atlas_path_open_nofollow(opts->root_fd, COMPILE_DB_NAME, strlen(COMPILE_DB_NAME), &res, &fd,
                                 &sb, NULL, &ignore) != ATLAS_OK ||
        res != ATLAS_PATH_OPEN_OK) {
        if (fd >= 0) {
            (void)close(fd);
        }
        /* No compile database is an ordinary state, never an error. Resolution
         * falls back to lexical matching and says so. */
        if (state->compile_db_present) {
            atlas_status st = atlas_db_code_units_clear(db, repo_id, err);
            if (st == ATLAS_OK) {
                st = atlas_db_code_state_set_compile_db(db, repo_id, false, NULL, 0, 0, err);
            }
            if (st != ATLAS_OK) {
                return st;
            }
            sum->compile_db_changed = true;
        }
        return ATLAS_OK;
    }

    atlas_buf content = ATLAS_BUF_INIT;
    bool over = false;
    atlas_status st = ATLAS_OK;
    for (;;) {
        char chunk[64u * 1024u];
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            st = atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno,
                                     "cannot read the compile database");
            break;
        }
        if (n == 0) {
            break;
        }
        if (content.len + (size_t)n > ATLAS_CODE_MAX_COMPILE_DB_BYTES) {
            over = true;
            break;
        }
        st = atlas_buf_append(&content, chunk, (size_t)n, err);
        if (st != ATLAS_OK) {
            break;
        }
    }
    (void)close(fd);
    if (st != ATLAS_OK) {
        atlas_buf_free(&content);
        return st;
    }

    char hash[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_sha256_hex(content.data, content.len, hash);
    bool changed = rebuild || !state->compile_db_present ||
                   strcmp(atlas_buf_cstr(&state->compile_db_hash), hash) != 0;
    if (!changed) {
        atlas_buf_free(&content);
        sum->compile_db_present = true;
        sum->compile_units = state->compile_units;
        return ATLAS_OK;
    }

    atlas_code_compdb cdb;
    st = atlas_code_compdb_parse(content.data, over ? 0 : content.len, opts->root_raw,
                                 opts->root_len, &cdb, err);
    atlas_buf_free(&content);
    if (st != ATLAS_OK) {
        atlas_code_compdb_free(&cdb);
        return st;
    }
    if (over) {
        cdb.truncated = true;
        cdb.truncated_reason = "the compile database exceeds the size Atlas will read";
    }

    st = atlas_db_code_units_clear(db, repo_id, err);
    for (size_t i = 0; st == ATLAS_OK && i < cdb.unit_count; i++) {
        const atlas_code_cu *cu = &cdb.units[i];
        atlas_buf text = ATLAS_BUF_INIT;
        const char *src = atlas_code_compdb_str(&cdb, cu->source_off);
        st = atlas_path_text_encode(src, cu->source_len, &text, err);
        atlas_code_unit_record ur;
        memset(&ur, 0, sizeof(ur));
        ur.source_path_raw = src;
        ur.source_path_len = cu->source_len;
        ur.source_path_text = atlas_buf_cstr(&text);
        ur.output_text = atlas_code_compdb_str(&cdb, cu->output_off);
        ur.directory_text = atlas_code_compdb_str(&cdb, cu->dir_off);
        ur.language_standard = cu->std_len > 0 ? atlas_code_compdb_str(&cdb, cu->std_off) : NULL;
        ur.explicit_language = cu->lang_len > 0 ? atlas_code_compdb_str(&cdb, cu->lang_off) : NULL;
        ur.arg_count = cu->arg_count;
        ur.dropped_args = cu->dropped_args;
        ur.command_present = cu->command_present;
        ur.command_hash = cu->command_present ? cu->command_hash : NULL;
        ur.entry_index = cu->entry_index;
        ur.generation = generation;
        int64_t unit_id = 0;
        if (st == ATLAS_OK) {
            st = atlas_db_code_unit_insert(db, repo_id, &ur, &unit_id, err);
        }
        atlas_buf_free(&text);
        for (size_t k = 0; st == ATLAS_OK && k < cu->incdir_count; k++) {
            const atlas_code_cu_incdir *d = &cdb.incdirs[cu->incdir_first + k];
            const char *dir = atlas_code_compdb_str(&cdb, d->path_off);
            atlas_buf dtext = ATLAS_BUF_INIT;
            st = atlas_path_text_encode(dir, d->path_len, &dtext, err);
            if (st == ATLAS_OK) {
                st = atlas_db_code_unit_include_add(
                    db, unit_id, atlas_code_incdir_kind_name((atlas_code_incdir_kind)d->kind), dir,
                    d->path_len, atlas_buf_cstr(&dtext), d->external, (int64_t)k, err);
            }
            atlas_buf_free(&dtext);
        }
        for (size_t k = 0; st == ATLAS_OK && k < cu->define_count; k++) {
            const atlas_code_cu_define *d = &cdb.defines[cu->define_first + k];
            st = atlas_db_code_unit_define_add(
                db, unit_id, atlas_code_compdb_str(&cdb, d->name_off),
                d->value_len > 0 ? atlas_code_compdb_str(&cdb, d->value_off) : NULL, d->undef,
                (int64_t)k, err);
        }
        /* The unit-to-file edges are deliberately *not* written here. On a first
         * pass there is no `code_files` row to point at yet, and on a later one
         * the includes are not resolved until the resolver has run — so both
         * kinds are rebuilt in one step after both, by
         * `atlas_db_code_link_units`. Writing them here would have produced an
         * edge set that was correct only from the second pass onward, which is
         * exactly the kind of bug that looks like intermittent flakiness. */
    }
    if (st == ATLAS_OK) {
        st = atlas_db_code_state_set_compile_db(db, repo_id, true, hash, (int64_t)cdb.unit_count,
                                                cdb.entries_dropped, err);
    }
    if (st == ATLAS_OK && cdb.truncated) {
        sum->truncated = true;
        sum->truncated_reason = cdb.truncated_reason;
        st = atlas_db_code_error_add(db, repo_id, COMPILE_DB_NAME, "compile_db_error",
                                     cdb.truncated_reason, generation, err);
    }
    sum->compile_db_present = true;
    sum->compile_db_changed = true;
    sum->compile_units = (int64_t)cdb.unit_count;
    atlas_code_compdb_free(&cdb);
    return st;
}

/* --- the pass -------------------------------------------------------------------- */

atlas_status atlas_code_pass_run(atlas_db *db, int64_t repo_id, int64_t generation,
                                 const atlas_code_pass_opts *opts, atlas_code_pass_summary *sum,
                                 atlas_err *err) {
    atlas_code_pass_opts defaults;
    atlas_code_pass_opts_init(&defaults);
    if (opts == NULL) {
        opts = &defaults;
    }
    int64_t started = monotonic_ms();

    /* Read the previous state *before* claiming the generation.
     * `atlas_db_code_state_begin` clears `resolve_settled`, so a read after it
     * always reports the flag as false and the pass can never skip resolution —
     * which is the whole reason the column exists. */
    atlas_code_index_state state;
    atlas_code_index_state_init(&state);
    atlas_status st = atlas_db_code_state_get(db, repo_id, &state, err);
    if (st != ATLAS_OK) {
        atlas_code_index_state_free(&state);
        return st;
    }

    /* An analyzer upgrade is a rebuild, whether or not one was asked for.
     *
     * Nothing else in the pass can notice it: the repository bytes are
     * identical, so selection finds no file to reparse; the compile database
     * hash is identical, so nothing is re-ingested; every generation lines up.
     * The graph is simply the output of an algorithm that has since been
     * corrected, and the only honest response is to produce it again. The
     * summary says so, and until the rebuild completes the graph is reported
     * stale rather than current. */
    bool analyzer_changed = state.present && !atlas_code_analyzer_matches(&state);
    bool rebuild = opts->rebuild || analyzer_changed;
    sum->analyzer_changed = analyzer_changed;

    int64_t analyzer_row = 0;
    st = atlas_db_begin(db, err);
    if (st == ATLAS_OK) {
        st = atlas_db_code_analyzer_intern(db, ATLAS_CODE_ANALYZER_ID,
                                           (int64_t)ATLAS_CODE_ANALYZER_VERSION, &analyzer_row,
                                           err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_code_state_begin(db, repo_id, generation, err);
    }
    if (st == ATLAS_OK && rebuild) {
        /* Structural rows only. `atlas_db_code_clear_repo` names `code_files`
         * and `code_units`, so the cascade reaches symbols, occurrences,
         * relations, candidates and roles and reaches nothing else: sessions,
         * change sets, recorded reasons, decisions, evidence, commits and the
         * file index are all untouched by a structural rebuild, and
         * `tests/test_code_analyzer.c` asserts it row by row. */
        st = atlas_db_code_clear_repo(db, repo_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_code_state_set_analyzer(db, repo_id, analyzer_row, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_commit(db, err);
    } else {
        atlas_db_rollback(db);
    }
    if (st != ATLAS_OK) {
        atlas_code_index_state_free(&state);
        return st;
    }

    apply_ctx ac;
    memset(&ac, 0, sizeof(ac));
    ac.db = db;
    ac.repo_id = repo_id;
    ac.generation = generation;
    ac.sum = sum;
    atlas_buf_init(&ac.path_text);
    atlas_buf_init(&ac.name_text);
    atlas_buf_init(&ac.touched_names);
    atlas_buf_init(&ac.parsed_files);

    todo_table table;
    todo_init(&table);

    /* ---- the compile database, before anything is resolved against it ---- */
    if (opts->root_fd >= 0) {
        st = atlas_db_begin(db, err);
        if (st == ATLAS_OK) {
            st = ingest_compile_db(db, repo_id, generation, opts, rebuild, &state, sum, err);
            if (st == ATLAS_OK) {
                st = atlas_db_commit(db, err);
            } else {
                atlas_db_rollback(db);
            }
        }
        if (st != ATLAS_OK) {
            goto done;
        }
    }

    /* ---- removals: paths that left the index or were tombstoned ---- */
    {
        remove_ctx rc;
        memset(&rc, 0, sizeof(rc));
        rc.ac = &ac;
        int64_t n = 0;
        st = atlas_db_code_files_to_remove(db, repo_id, remove_note, &rc, &n, err);
        for (size_t i = 0; st == ATLAS_OK && i < rc.count; i++) {
            st = batch_begin(&ac, err);
            if (st == ATLAS_OK) {
                /* The names the departing file defined are remembered first, so
                 * the edges elsewhere that resolved to them are re-resolved
                 * rather than left pointing at a row that no longer exists. */
                st = remember_old_definitions(&ac, rc.ids[i], err);
            }
            if (st == ATLAS_OK) {
                /* Same targeted invalidation as a reparse, plus the file node:
                 * an include that resolved to this path has nothing to point at
                 * once the row is gone. */
                int64_t unsettled = 0;
                st = atlas_db_code_relations_unsettle_for_file(db, repo_id, rc.ids[i], true,
                                                               generation, &unsettled, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_db_code_file_delete(db, rc.ids[i], err);
            }
            if (st == ATLAS_OK) {
                sum->files_removed++;
                ac.file_set_changed = true;
                st = batch_maybe_commit(&ac, false, err);
            }
        }
        if (st == ATLAS_OK) {
            st = batch_maybe_commit(&ac, true, err);
        } else {
            atlas_db_rollback(db);
            ac.in_batch = 0;
        }
        free(rc.ids);
        if (st != ATLAS_OK) {
            goto done;
        }
    }

    /* ---- select, parse and apply, in chunks ----
     *
     * The three stages keep their order and their rules — no I/O in select, no
     * transaction during parse, no parse inside a transaction — and they run
     * over a bounded slice of the work at a time rather than over all of it.
     *
     * That bound is the point. A parse result is a few kilobytes, so gathering
     * twenty thousand of them before applying any is hundreds of megabytes held
     * for no purpose: the writer applies them one at a time regardless. Chunking
     * makes the stage's memory a property of ATLAS_CODE_PARSE_CHUNK rather than
     * of the repository, which is the bound Atlas claims everywhere else.
     *
     * The loop needs no cursor. Selection compares content hashes, and applying
     * a file updates the hash its facts were built from — so a file that has
     * been applied is no longer selected, and re-running the query returns the
     * next slice. */
    {
        int64_t budget = opts->max_files > 0 ? opts->max_files
                                             : ATLAS_CODE_MAX_PARSE_FILES_PER_PASS;
        bool more = false;
        while (st == ATLAS_OK && budget > 0) {
            int64_t chunk = budget < ATLAS_CODE_PARSE_CHUNK ? budget : ATLAS_CODE_PARSE_CHUNK;
            todo_free(&table);
            todo_init(&table);
            select_ctx sc;
            memset(&sc, 0, sizeof(sc));
            sc.table = &table;
            sc.limit = chunk;
            int64_t n = 0;
            bool chunk_more = false;
            st = atlas_db_code_files_to_parse(db, repo_id, chunk, todo_add, &sc, &n, &chunk_more,
                                              err);
            if (st != ATLAS_OK || table.count == 0) {
                more = more || chunk_more;
                break;
            }
            sum->files_selected += (int64_t)table.count;
            budget -= (int64_t)table.count;
            more = chunk_more;

            parse_ctx pc;
            memset(&pc, 0, sizeof(pc));
            pc.table = &table;
            pc.root_fd = opts->root_fd;
            st = atlas_workers_for_each(opts->workers, table.count, parse_job, &pc, err);
            if (st != ATLAS_OK) {
                break;
            }

            for (size_t i = 0; st == ATLAS_OK && i < table.count; i++) {
                st = batch_begin(&ac, err);
                if (st != ATLAS_OK) {
                    break;
                }
                st = apply_file(&ac, &table, &table.items[i], err);
                if (st != ATLAS_OK) {
                    break;
                }
                st = batch_maybe_commit(&ac, false, err);
            }
            if (st == ATLAS_OK) {
                st = batch_maybe_commit(&ac, true, err);
            } else {
                atlas_db_rollback(db);
                ac.in_batch = 0;
            }
        }
        if (st != ATLAS_OK) {
            goto done;
        }
        if (more || budget <= 0) {
            /* Never a silent stop: the remainder still differs by content hash,
             * so the next pass finds it, and this one says it did not finish. */
            sum->truncated = true;
            sum->truncated_reason = "more files changed than one structural pass parses; the "
                                    "remainder is parsed by the next pass";
            st = atlas_db_code_error_add(db, repo_id, NULL, "pass_truncated",
                                         sum->truncated_reason, generation, err);
            if (st != ATLAS_OK) {
                goto done;
            }
        }
    }
    /* ---- resolve ----
     *
     * Only when something this pass did could change an answer.
     *
     * Resolution is not cheap and it is not incremental in the way parsing is:
     * a sweep re-attempts every edge that is still UNRESOLVED or AMBIGUOUS,
     * because one of them may have become resolvable. But an unresolved edge
     * becomes resolvable only when the candidate universe changes — a file
     * appears or leaves, a definition is added or removed, the compile database
     * changes — and every one of those makes this pass parse or remove
     * something. Asking again when nothing changed puts the same question to the
     * same rows and gets the same answer; at five thousand files that was a
     * minute of work per pass, on a repository nobody had touched.
     *
     * The durable `resolve_settled` flag is what makes skipping safe across a
     * crash: it is cleared at the start of every pass and set only at the end,
     * so a pass that died during resolution leaves it false and the next pass
     * resolves whatever this reasoning would otherwise have skipped. */
    bool scope_unknown = !state.resolve_settled;
    bool resolve_needed = rebuild || sum->compile_db_changed || sum->files_parsed > 0 ||
                          sum->files_removed > 0 || scope_unknown;
    if (resolve_needed) {
        /* No transaction wraps this. The resolver opens and commits its own, in
         * bounded pages: resolution touches every unsettled edge in the
         * repository, and a transaction is never held across unbounded work —
         * it would hold the write lock for the length of the pass and grow the
         * uncommitted page set with the repository rather than with a
         * constant. */
        atlas_code_resolve_scope scope;
        memset(&scope, 0, sizeof(scope));
        scope.names = ac.touched_names.data;
        scope.names_len = ac.touched_names.len;
        scope.files = (const int64_t *)(const void *)ac.parsed_files.data;
        scope.file_count = ac.parsed_files.len / sizeof(int64_t);
        scope.file_set_changed = ac.file_set_changed || sum->compile_db_changed;
        /* Three ways the incremental scope stops being trustworthy, and all
         * three mean the same thing: sweep the repository.
         *
         *   - a rebuild was asked for;
         *   - the change set overflowed its own bound, so what it lists is a
         *     prefix rather than a description;
         *   - `resolve_settled` was false on entry. That is a first pass, or a
         *     pass that died during resolution. In the second case some edges
         *     were settled under rules that have since changed and no longer
         *     look unsettled, so re-attempting only the unsettled ones would
         *     leave exactly the wrong rows behind.
         *
         * Only the middle one is a *fallback* — the other two are the pass
         * doing what was asked. Reporting a first index as a fallback would
         * put a degradation notice on every new repository. */
        scope.full = rebuild || scope_unknown || ac.touched_overflow || ac.parsed_overflow;
        if ((ac.touched_overflow || ac.parsed_overflow) && !scope_unknown && !rebuild) {
            sum->resolve_fallback = true;
        }
        st = atlas_code_resolve(db, repo_id, generation, &scope, sum, err);
        if (st != ATLAS_OK) {
            goto done;
        }
    }

    /* ---- link the compile units, once everything they point at exists ----
     *
     * Guarded by the same condition, and it has to be. `unit_uses_header` is
     * *derived* from resolved include edges, so it goes stale whenever those
     * change — which resolution can do with no file parsed at all, when a
     * previously unresolvable include finds its header. Tying the relink to
     * "did resolution run" rather than to "were files parsed" is what keeps the
     * derived edges and the edges they derive from in step.
     *
     * Skipping it matters because it is not a no-op when nothing changed: the
     * implementation deletes every unit edge and reinserts it, which at five
     * thousand units rewrote tens of thousands of rows per pass.
     *
     * `scope_unknown` is in the condition and has to be, for a reason that only
     * shows up on the crash path. A unit edge is *owned by* the unit's source
     * file, so `code_file_clear` deletes it along with everything else that file
     * owns — and the pass that reparsed the file is the one that puts it back.
     * If that pass died between the two, the recovery pass parses nothing, has
     * no file to relink by id, and would leave the unit edges durably missing
     * with nothing ever noticing. The same flag that says "re-resolve
     * everything" has to say "relink everything", because the same dead pass
     * damaged both. */
    if (resolve_needed) {
        /* Whole-repository only when the compile database or the file set moved:
         * then any unit may have changed, and the units are not addressable one
         * at a time. Otherwise one indexed rebuild per file this pass parsed —
         * a unit's header edges derive from its own source file's includes, so
         * no other unit can have been affected. */
        bool link_all = rebuild || sum->compile_db_changed || ac.file_set_changed ||
                        ac.parsed_overflow || scope_unknown;
        size_t parsed_n = ac.parsed_files.len / sizeof(int64_t);
        const int64_t *parsed_ids = (const int64_t *)(const void *)ac.parsed_files.data;
        st = atlas_db_begin(db, err);
        for (size_t i = 0; st == ATLAS_OK && i < (link_all ? 1u : parsed_n); i++) {
            int64_t linked = 0;
            st = atlas_db_code_link_units(db, repo_id, link_all ? 0 : parsed_ids[i], generation,
                                          &linked, err);
            if (st == ATLAS_OK) {
                sum->relations_written += linked;
            }
        }
        if (st == ATLAS_OK) {
            st = atlas_db_commit(db, err);
        } else {
            atlas_db_rollback(db);
        }
        if (st != ATLAS_OK) {
            goto done;
        }
    }

    /* ---- publish ---- */
    {
        if (sum->files_failed > 0) {
            sum->degraded = true;
            sum->degraded_reason = "a file could not be structurally parsed";
        } else if (sum->truncated) {
            sum->degraded = true;
            sum->degraded_reason = "a structural indexing ceiling was reached";
        }
        st = atlas_db_begin(db, err);
        if (st == ATLAS_OK && sum->resolve_fallback) {
            st = atlas_db_code_error_add(db, repo_id, NULL, "resolve_fallback",
                                         "more symbol names changed than one incremental "
                                         "resolution tracks; the repository was re-resolved",
                                         generation, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_db_code_state_complete(db, repo_id, generation, sum->files_parsed,
                                              sum->degraded, sum->degraded_reason,
                                              sum->truncated ? sum->truncated_reason : NULL,
                                              resolve_needed, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_db_code_errors_prune(db, repo_id, ATLAS_CODE_ERRORS_RETAIN_PER_REPO, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_db_commit(db, err);
        } else {
            atlas_db_rollback(db);
        }
    }

done:
    sum->duration_ms = monotonic_ms() - started;
    atlas_buf_free(&ac.path_text);
    atlas_buf_free(&ac.name_text);
    atlas_buf_free(&ac.touched_names);
    atlas_buf_free(&ac.parsed_files);
    todo_free(&table);
    atlas_code_index_state_free(&state);
    return st;
}

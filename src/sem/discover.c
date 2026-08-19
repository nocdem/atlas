/* Atlas - A9.2.4: the bounded walk that finds a repository's compilation
 * databases, and the identity that makes a change to that set a rebuild.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The whole file is a filesystem read. It opens no database handle, creates no
 * process, takes no lock and writes no row — the property `atlas_gate_check` and
 * `atlas_sem_plan_for` have, and for the same reason: a caller must be able to
 * ask what Atlas would find without that question changing anything. Persisting
 * a result is `src/db/db_sem.c`'s business and scheduling one is the daemon's;
 * neither belongs here.
 *
 * The argument for every bound, and for descending the way this descends, is in
 * `include/atlas/sem_discover.h`. What is worth repeating beside the code is the
 * one thing a later edit would be most tempted to simplify away:
 *
 *   A candidate that cannot be read, is a symlink, is too large or does not
 *   parse is **recorded as a rejected candidate with a reason**. It never fails
 *   the pass, and it is never silently skipped. Failing the pass would let one
 *   unreadable file erase a repository's whole build description; skipping
 *   silently would leave a hole in the search universe that nothing reports —
 *   and this season exists because a hole in the search universe went unnoticed.
 */
#include "atlas/sem_discover.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/pathrep.h"
#include "atlas/sha256.h"

/* The one filename Atlas understands as a build description.
 *
 * Fixed rather than configurable, and that is deliberate: the format Atlas can
 * read is the JSON Compilation Database, the name that format is published under
 * is this one, and letting a repository nominate other names would widen the set
 * of files a walk opens on the repository's own say-so. An operator who has one
 * under another name pins it with `--compdb`, which is an operator naming a file
 * rather than a repository doing so. */
static const char COMPDB_NAME[] = "compile_commands.json";

const char *atlas_sem_input_origin_name(atlas_sem_input_origin o) {
    switch (o) {
    case ATLAS_SEM_INPUT_PINNED:
        return "PINNED";
    case ATLAS_SEM_INPUT_DISCOVERED:
        return "DISCOVERED";
    case ATLAS_SEM_INPUT_BOTH:
        return "BOTH";
    case ATLAS_SEM_INPUT_UNKNOWN:
        break;
    }
    return "UNKNOWN";
}

bool atlas_sem_input_origin_parse(const char *name, atlas_sem_input_origin *out) {
    if (name == NULL || out == NULL) {
        return false;
    }
    static const struct {
        const char *name;
        atlas_sem_input_origin v;
    } TABLE[] = {
        {"UNKNOWN", ATLAS_SEM_INPUT_UNKNOWN},
        {"PINNED", ATLAS_SEM_INPUT_PINNED},
        {"DISCOVERED", ATLAS_SEM_INPUT_DISCOVERED},
        {"BOTH", ATLAS_SEM_INPUT_BOTH},
    };
    for (size_t i = 0; i < sizeof TABLE / sizeof TABLE[0]; i++) {
        if (strcmp(name, TABLE[i].name) == 0) {
            *out = TABLE[i].v;
            return true;
        }
    }
    return false;
}

const char *atlas_sem_reject_intern(const char *reason) {
    static const char *const REASONS[] = {
        ATLAS_SEM_REJECT_EXCLUDED,  ATLAS_SEM_REJECT_NOT_REGULAR, ATLAS_SEM_REJECT_SYMLINK,
        ATLAS_SEM_REJECT_TOO_LARGE, ATLAS_SEM_REJECT_MALFORMED,   ATLAS_SEM_REJECT_DUPLICATE,
        ATLAS_SEM_REJECT_TOO_MANY,
    };
    if (reason == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof REASONS / sizeof REASONS[0]; i++) {
        if (strcmp(reason, REASONS[i]) == 0) {
            return REASONS[i];
        }
    }
    return NULL;
}

bool atlas_sem_reject_reason_is_known(const char *reason) {
    return atlas_sem_reject_intern(reason) != NULL;
}

void atlas_sem_discovery_result_init(atlas_sem_discovery_result *r) {
    if (r == NULL) {
        return;
    }
    memset(r, 0, sizeof(*r));
    /* Everything a memset leaves is the safe reading: UNKNOWN discovery, no
     * accepted input, no limit reached. A zeroed result never asserts that a
     * search universe was covered. */
}

/* --- the candidate table ---------------------------------------------------- */

/* Identity for deduplication, and the reason it is not the path.
 *
 * One file reachable through a symlink, a relative path and its canonical path
 * must not become three semantic inputs. A path comparison cannot see that;
 * `(device, inode)` can, and it is what the kernel means by "the same file". The
 * path is still what is *reported*, because an operator needs to know which name
 * Atlas used. */
typedef struct cand_ident {
    dev_t dev;
    ino_t ino;
    bool known;
} cand_ident;

typedef struct walk_state {
    atlas_sem_discovery_result *out;
    cand_ident idents[ATLAS_SEM_DISCOVERY_MAX_CANDIDATES];
    const char *packed_excludes;
    const char *root;
    /* A9.2.6 successor. Offered every `ATLAS_SEM_DISCOVER_YIELD_EVERY` entries
     * so the thread running this walk can be lent to something short. NULL when
     * nobody supplied one, which is every caller that is not the daemon's writer
     * thread — a status command walks with nothing to lend the thread to.
     *
     * Safe here for the reason it is safe between translation units: the whole
     * walk runs before any transaction opens. Nothing in this file touches a
     * database. */
    void (*yield)(void *ud);
    void *yield_ud;
    int64_t entries;
    /* Abandon the rest of the walk. Set only by the two ceilings that bound
     * *total* work — entries read and candidates found — because those are the
     * ones where continuing would defeat the bound.
     *
     * Kept apart from `limit_reached`, and the separation is not cosmetic: an
     * earlier cut used one flag for both, so an operator's ordinary `--exclude`
     * recorded a reason, set the flag, and silently abandoned every
     * not-yet-visited sibling directory. The walk returned PARTIAL, which was
     * true, and found one compilation database instead of two, which was not
     * what anybody asked for. A per-branch limit — depth, an over-long path, a
     * directory with more subdirectories than one level holds — records its
     * reason and the walk carries on elsewhere. */
    bool stop;
} walk_state;

/* Records the first reason the search fell short of its universe.
 *
 * A ceiling is one such reason and it is not the only one: a directory Atlas
 * could not enter leaves exactly as big a hole and is exactly as invisible to a
 * reader. Both go here, because what an operator needs is not "a bound was hit"
 * but *why this search cannot be called complete*, and a PARTIAL verdict with no
 * reason tells somebody something was missed without telling them what.
 *
 * The first, because it is the one to act on; a walk that could not enter one
 * directory will usually not enter its siblings either. Takes a finished string
 * rather than a format, so no repository-derived byte can ever reach a format
 * string — these describe files whose names a repository chose. */
static void note_partial(walk_state *w, const char *detail) {
    w->out->state = ATLAS_SEM_DISC_PARTIAL;

    if (w->out->limit_reached) {
        return;
    }
    w->out->limit_reached = true;
    (void)snprintf(w->out->limit_detail, sizeof w->out->limit_detail, "%s", detail);
}

/* A9.2.5. Records one obstacle **with the path it is about**.
 *
 * `note_partial` above keeps its one-line summary because existing readers
 * consume it, and it is still the *first* reason — but it is no longer the only
 * thing recorded, which is the defect. A single `--exclude` used to consume the
 * one slot and mask every unreadable directory for the rest of the walk.
 *
 * The path is `%XX`-encoded, exactly as a rejected candidate's is: the argument
 * that a repository-chosen path must not reach an operator was already answered
 * by `encode_rel`, twelve lines from here, for files. The reason stays a
 * separate column and is never concatenated into the path — a value an operator
 * reads must stay one Atlas owns.
 *
 * Reaching the bound is itself reported. A list silently trimmed would recreate
 * the invisible hole this exists to close. */
static void note_obstacle(walk_state *w, const char *rel_text, const char *reason) {
    w->out->state = ATLAS_SEM_DISC_PARTIAL;
    const char *path = rel_text != NULL && rel_text[0] != '\0' ? rel_text : ".";

    /* Deduplicated on `(path, reason)`, and that is not tidiness.
     *
     * Two of the call sites are inside the `readdir` loop and fire per *entry*:
     * one directory holding forty unrepresentable names would otherwise consume
     * all thirty-two slots with thirty-two byte-identical rows and hide every
     * other obstacle in the tree. That is the A9.2.4 failure mode — one reason
     * consuming the only slot — with the slot count raised from one to
     * thirty-two rather than the masking removed. Bounded linear scan over at
     * most `ATLAS_SEM_DISCOVERY_MAX_OBSTACLES` entries. */
    for (size_t i = 0; i < w->out->obstacle_count; i++) {
        if (strcmp(w->out->obstacles[i].path, path) == 0 &&
            strcmp(w->out->obstacles[i].reason, reason) == 0) {
            return;
        }
    }
    if (w->out->obstacle_count >= ATLAS_SEM_DISCOVERY_MAX_OBSTACLES) {
        w->out->obstacles_truncated = true;
        return;
    }
    atlas_sem_obstacle *o = &w->out->obstacles[w->out->obstacle_count++];
    /* The repository root itself is the empty relative path; naming it "." is
     * what an operator would type. */
    (void)snprintf(o->path, sizeof o->path, "%s", path);
    (void)snprintf(o->reason, sizeof o->reason, "%s", reason);
}

/* Both, in the order a reader wants them: the exact place first, then the
 * one-line summary that existing surfaces already print. */
static void note_partial_at(walk_state *w, const char *rel_text, const char *reason,
                            const char *detail) {
    note_obstacle(w, rel_text, reason);
    note_partial(w, detail);
}

/* Finds an existing candidate by reported path, or appends one.
 *
 * Returns NULL when the candidate ceiling is reached, having recorded the limit:
 * a candidate that does not fit is not dropped quietly, it makes the whole walk
 * PARTIAL. */
static atlas_sem_input *intern_path(walk_state *w, const char *path) {
    for (size_t i = 0; i < w->out->count; i++) {
        if (strcmp(w->out->inputs[i].path, path) == 0) {
            return &w->out->inputs[i];
        }
    }
    if (w->out->count >= ATLAS_SEM_DISCOVERY_MAX_CANDIDATES) {
        char detail[128];
        (void)snprintf(detail, sizeof detail,
                       "more than %d candidate compilation databases were found",
                       ATLAS_SEM_DISCOVERY_MAX_CANDIDATES);
        note_partial_at(w, path, ATLAS_SEM_OBSTACLE_CANDIDATES, detail);
        w->stop = true;
        return NULL;
    }
    atlas_sem_input *in = &w->out->inputs[w->out->count];
    memset(in, 0, sizeof(*in));
    memset(&w->idents[w->out->count], 0, sizeof w->idents[0]);
    (void)snprintf(in->path, sizeof in->path, "%s", path);
    w->out->count++;
    return in;
}

static void reject(atlas_sem_input *in, const char *reason) {
    (void)snprintf(in->reject_reason, sizeof in->reject_reason, "%s", reason);
}

/* --- reading and validating one candidate ------------------------------------ */

/* `dir_fd` is a descriptor for the directory holding `name`, obtained by the
 * walk's own `O_NOFOLLOW` descent or by the pinned path's guarded resolution.
 * Nothing here re-resolves a path from a string, which is A8's workspace rule
 * applied to a read. */
static void validate(walk_state *w, int dir_fd, const char *name, atlas_sem_input *in) {
    struct stat sb;
    memset(&sb, 0, sizeof sb);
    if (fstatat(dir_fd, name, &sb, AT_SYMLINK_NOFOLLOW) != 0) {
        reject(in, ATLAS_SEM_REJECT_NOT_REGULAR);
        return;
    }
    if (S_ISLNK(sb.st_mode)) {
        /* Never followed, and reported rather than skipped. In the repository
         * this season was developed in, the top-level `compile_commands.json` is
         * a link into `build/` — so this is the ordinary case, not an exotic one,
         * and an operator seeing the link named as a rejected candidate is
         * seeing the truth: the file it points at is discovered on its own. */
        reject(in, ATLAS_SEM_REJECT_SYMLINK);
        return;
    }
    if (!S_ISREG(sb.st_mode)) {
        reject(in, ATLAS_SEM_REJECT_NOT_REGULAR);
        return;
    }

    /* Deduplicate by what the kernel means by "the same file". */
    size_t slot = (size_t)(in - w->out->inputs);
    for (size_t i = 0; i < w->out->count; i++) {
        if (i == slot || !w->idents[i].known || !w->out->inputs[i].accepted) {
            continue;
        }
        if (w->idents[i].dev == sb.st_dev && w->idents[i].ino == sb.st_ino) {
            reject(in, ATLAS_SEM_REJECT_DUPLICATE);
            return;
        }
    }

    int fd = openat(dir_fd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        reject(in, ATLAS_SEM_REJECT_NOT_REGULAR);
        return;
    }
    atlas_buf data = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    bool too_large = false;
    bool read_failed = false;
    for (;;) {
        char chunk[64u * 1024u];
        ssize_t n = read(fd, chunk, sizeof chunk);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            read_failed = true;
            break;
        }
        if (n == 0) {
            break;
        }
        if (data.len + (size_t)n > ATLAS_CODE_MAX_COMPILE_DB_BYTES) {
            /* Refused, never truncated: half a compilation database describes a
             * different repository and nothing in the result would say so. */
            too_large = true;
            break;
        }
        if (atlas_buf_append(&data, chunk, (size_t)n, &err) != ATLAS_OK) {
            read_failed = true;
            break;
        }
    }
    (void)close(fd);

    if (too_large) {
        reject(in, ATLAS_SEM_REJECT_TOO_LARGE);
        atlas_buf_free(&data);
        return;
    }
    if (read_failed) {
        reject(in, ATLAS_SEM_REJECT_NOT_REGULAR);
        atlas_buf_free(&data);
        return;
    }

    atlas_code_compdb parsed;
    atlas_code_compdb_init(&parsed);
    atlas_status st =
        atlas_code_compdb_parse(data.data, data.len, w->root, strlen(w->root), &parsed, &err);
    /* `atlas_code_compdb_parse` reports a malformed document as an ordinary
     * outcome — zero units and a reason — rather than as a status, because a
     * repository must not be able to take a pass down by writing bad JSON. So
     * the test is *what it produced*, not what it returned.
     *
     * Truncated with zero units means Atlas could not read it as a compilation
     * database at all: not JSON, not an array, or empty. Truncated *with* units
     * means it read as many as it records and hit a ceiling, which the indexer
     * reports on its own and which does not make the file unusable.
     *
     * Zero units without truncation is neither: an empty compilation database is
     * a build that has produced nothing yet, which is a different fact from one
     * Atlas could not read, and the two must stay different. */
    if (st != ATLAS_OK || (parsed.truncated && parsed.unit_count == 0)) {
        reject(in, ATLAS_SEM_REJECT_MALFORMED);
        atlas_code_compdb_free(&parsed);
        atlas_buf_free(&data);
        return;
    }

    atlas_sha256_hex(data.data, data.len, in->digest);
    /* Zero units is *not* an error and is not a rejection. An empty compilation
     * database is a build that has produced nothing yet, which is a different
     * fact from one Atlas could not read, and the two must stay different. */
    in->unit_count = (int64_t)parsed.unit_count;
    in->accepted = true;
    w->idents[slot].dev = sb.st_dev;
    w->idents[slot].ino = sb.st_ino;
    w->idents[slot].known = true;
    w->out->accepted_count++;

    atlas_code_compdb_free(&parsed);
    atlas_buf_free(&data);
}

static void mark_origin(atlas_sem_input *in, atlas_sem_input_origin origin) {
    if (in->origin == ATLAS_SEM_INPUT_UNKNOWN) {
        in->origin = origin;
    } else if (in->origin != origin) {
        in->origin = ATLAS_SEM_INPUT_BOTH;
    }
}

/* --- the pinned list ---------------------------------------------------------- */

/* Opens a repository-relative *directory*, descending one component at a time
 * with `O_NOFOLLOW`, never following a symlink and never leaving the root.
 *
 * `atlas_path_open_nofollow` is the wrong tool here: it is built for files and
 * reports a directory as NOT_REGULAR, so a pinned path under a build directory
 * would be refused for existing in one. The descent below is the same
 * discipline applied to the thing the walk itself descends — every `openat` from
 * a descriptor validated once, never a path re-resolved from a string.
 *
 * Returns -1 for anything at all that is not a plain directory reachable this
 * way. `..` and an absolute path are refused before the first `openat`: a build
 * description naming `../../etc` is a repository choosing what Atlas opens. */
static int open_dir_nofollow(int root_fd, const char *rel) {
    if (rel == NULL || rel[0] == '\0' || rel[0] == '/') {
        return -1;
    }
    int fd = dup(root_fd);
    if (fd < 0) {
        return -1;
    }
    const char *p = rel;
    while (*p != '\0') {
        const char *slash = strchr(p, '/');
        size_t n = slash != NULL ? (size_t)(slash - p) : strlen(p);
        if (n == 0 || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) {
            (void)close(fd);
            return -1;
        }
        char comp[256];
        if (n >= sizeof comp) {
            (void)close(fd);
            return -1;
        }
        memcpy(comp, p, n);
        comp[n] = '\0';
        int next = openat(fd, comp, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        (void)close(fd);
        if (next < 0) {
            return -1;
        }
        fd = next;
        p = slash != NULL ? slash + 1 : p + n;
    }
    return fd;
}

/* A path an operator named. Resolved with `atlas_path_open_nofollow`, exactly as
 * `--compdb` always was, so a pinned path cannot reach outside the repository
 * and cannot be pointed there by a planted link. */
static void take_pinned(walk_state *w, int root_fd, const char *rel) {
    atlas_sem_input *in = intern_path(w, rel);
    if (in == NULL) {
        return;
    }
    mark_origin(in, ATLAS_SEM_INPUT_PINNED);
    if (in->accepted || in->reject_reason[0] != '\0') {
        return; /* already resolved by the walk */
    }
    if (atlas_sem_path_under_prefix(w->packed_excludes, rel)) {
        /* An operator pinned a path and excluded the subtree it is in. Reported
         * rather than resolved one way or the other: Atlas does not guess which
         * of two operator statements was meant. */
        reject(in, ATLAS_SEM_REJECT_EXCLUDED);
        return;
    }

    /* Split the pinned path into the directory to descend and the final name, so
     * `validate` receives a descriptor rather than a re-resolved string. */
    const char *slash = strrchr(rel, '/');
    if (slash == NULL) {
        validate(w, root_fd, rel, in);
        return;
    }
    char dir[ATLAS_SEM_MAX_PATH_BYTES];
    size_t dir_len = (size_t)(slash - rel);
    if (dir_len >= sizeof dir) {
        reject(in, ATLAS_SEM_REJECT_NOT_REGULAR);
        return;
    }
    memcpy(dir, rel, dir_len);
    dir[dir_len] = '\0';
    int fd = open_dir_nofollow(root_fd, dir);
    if (fd < 0) {
        /* Missing, unreadable, not a directory, or a symlinked component —
         * which is never followed, so a pinned path cannot be pointed outside
         * the repository by planting a link where a build directory is
         * expected. */
        reject(in, ATLAS_SEM_REJECT_NOT_REGULAR);
        return;
    }
    validate(w, fd, slash + 1, in);
    (void)close(fd);
}

/* --- the walk ---------------------------------------------------------------- */

/* Encodes a repository-relative raw path into the reported text form, refusing
 * one that does not fit. Refused rather than truncated, for A5's reason about
 * `--older-than`: a truncated path names a different file. */
static bool encode_rel(const char *raw, size_t len, char out[ATLAS_SEM_MAX_PATH_BYTES]) {
    atlas_buf enc = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    out[0] = '\0';
    bool ok = atlas_path_text_encode(raw, len, &enc, &err) == ATLAS_OK &&
              enc.len < ATLAS_SEM_MAX_PATH_BYTES;
    if (ok) {
        memcpy(out, enc.data, enc.len);
        out[enc.len] = '\0';
    }
    atlas_buf_free(&enc);
    return ok;
}

static void walk_dir(walk_state *w, int dir_fd, const char *rel, size_t rel_len,
                     const char *rel_text, int depth);

static void descend(walk_state *w, int dir_fd, const char *name, const char *rel, size_t rel_len,
                    const char *rel_text, int depth) {
    /* The child's encoded path first, so every obstacle below can name the place
     * it is about. A9.2.4 recorded these reasons without a path on the grounds
     * that a path is bytes a repository chose — but `encode_rel` is the answer to
     * that and every accepted and rejected candidate already carries one. */
    char child[ATLAS_SEM_MAX_PATH_BYTES];
    int n = rel_len == 0 ? snprintf(child, sizeof child, "%s", name)
                         : snprintf(child, sizeof child, "%s/%s", rel, name);
    char child_text[ATLAS_SEM_MAX_PATH_BYTES];
    bool named = n > 0 && (size_t)n < sizeof child &&
                 encode_rel(child, (size_t)n, child_text);

    if (depth + 1 > ATLAS_SEM_DISCOVERY_MAX_DEPTH) {
        char detail[128];
        (void)snprintf(detail, sizeof detail, "the directory walk reached its depth ceiling of %d",
                       ATLAS_SEM_DISCOVERY_MAX_DEPTH);
        note_partial_at(w, named ? child_text : rel_text, ATLAS_SEM_OBSTACLE_DEPTH, detail);
        return;
    }
    if (!named) {
        note_partial_at(w, rel_text, ATLAS_SEM_OBSTACLE_PATH_TOO_LONG,
                        "a directory path exceeded the path ceiling");
        return;
    }
    /* `O_NOFOLLOW` on the descent is the whole traversal argument: a symlinked
     * directory is never entered, so no walk leaves the repository and no
     * repository can point one somewhere by planting a link. */
    int fd = openat(dir_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        /* Unreadable, a symlink, or not a directory after all. Atlas did not look
         * inside, so the universe it can vouch for is smaller than the one it set
         * out to cover — which is exactly what PARTIAL means. **With the path**,
         * since A9.2.5: "a directory could not be entered" without saying which
         * is not something an operator can act on, and on the repository that
         * produced this season it was masked entirely by an earlier reason. */
        note_partial_at(w, child_text, ATLAS_SEM_OBSTACLE_UNREADABLE_DIR,
                        "a directory could not be entered, so its contents are unaccounted for");
        return;
    }
    walk_dir(w, fd, child, (size_t)n, child_text, depth + 1);
    (void)close(fd);
}

/* Subdirectory names one directory level holds while its entries are read.
 *
 * The walk reads a directory to completion before descending, because holding a
 * `DIR *` open per level for the whole descent would consume a descriptor per
 * level of depth. Reached ⇒ PARTIAL, like every other ceiling here. */
#define WALK_MAX_SUBDIRS 256
#define WALK_MAX_NAME 256

static void walk_dir(walk_state *w, int dir_fd, const char *rel, size_t rel_len,
                     const char *rel_text, int depth) {
    /* `fdopendir` takes ownership of the descriptor it is given, so it gets a
     * duplicate and the caller's lifetime stays the caller's business. */
    int dup_fd = dup(dir_fd);
    if (dup_fd < 0) {
        note_partial_at(w, rel_text, ATLAS_SEM_OBSTACLE_UNREADABLE_DIR,
                        "a directory could not be opened for reading");
        return;
    }
    DIR *d = fdopendir(dup_fd);
    if (d == NULL) {
        (void)close(dup_fd);
        note_partial_at(w, rel_text, ATLAS_SEM_OBSTACLE_UNREADABLE_DIR,
                        "a directory could not be opened for reading");
        return;
    }
    w->out->dirs_visited++;

    /* Heap rather than a stack array: the walk recurses to
     * `ATLAS_SEM_DISCOVERY_MAX_DEPTH`, and a 64 KiB frame per level would make
     * the depth ceiling a stack bound in disguise. */
    size_t sub_count = 0;
    bool sub_overflow = false;
    char(*names)[WALK_MAX_NAME] = malloc((size_t)WALK_MAX_SUBDIRS * WALK_MAX_NAME);
    if (names == NULL) {
        (void)closedir(d);
        note_partial_at(w, rel_text, ATLAS_SEM_OBSTACLE_MEMORY,
                        "there was not enough memory to walk a directory");
        return;
    }

    for (;;) {
        errno = 0;
        struct dirent *e = readdir(d);
        if (e == NULL) {
            if (errno != 0) {
                note_partial_at(w, rel_text, ATLAS_SEM_OBSTACLE_UNREADABLE_ENTRIES,
                                "a directory could not be read to the end");
            }
            break;
        }
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        w->entries++;
        w->out->entries_seen++;
        /* Counted in entries read rather than in candidates found or files
         * opened: a tree of empty directories reads entries and opens nothing,
         * and an offer that depended on either would never be made there — which
         * is precisely the shape of the build directories this walk exists to
         * find its way through. */
        if (w->yield != NULL && w->entries % ATLAS_SEM_DISCOVER_YIELD_EVERY == 0) {
            w->yield(w->yield_ud);
        }
        if (w->entries > ATLAS_SEM_DISCOVERY_MAX_ENTRIES) {
            char detail[128];
            (void)snprintf(detail, sizeof detail, "the directory walk read more than %d entries",
                           ATLAS_SEM_DISCOVERY_MAX_ENTRIES);
            note_partial_at(w, rel_text, ATLAS_SEM_OBSTACLE_ENTRIES, detail);
            w->stop = true;
            break;
        }
        /* Git metadata is never a build description and is never entered. Not a
         * heuristic: `.git` is the one directory whose meaning Atlas already
         * knows, and a hostile configuration inside it is what
         * `src/git/git_harden.c` exists to keep away from. */
        if (strcmp(e->d_name, ".git") == 0) {
            continue;
        }

        char child_raw[ATLAS_SEM_MAX_PATH_BYTES];
        int n = rel_len == 0 ? snprintf(child_raw, sizeof child_raw, "%s", e->d_name)
                             : snprintf(child_raw, sizeof child_raw, "%s/%s", rel, e->d_name);
        if (n < 0 || (size_t)n >= sizeof child_raw) {
            note_partial_at(w, rel_text, ATLAS_SEM_OBSTACLE_PATH_TOO_LONG,
                            "a path exceeded the path ceiling");
            continue;
        }
        char child_text[ATLAS_SEM_MAX_PATH_BYTES];
        if (!encode_rel(child_raw, (size_t)n, child_text)) {
            /* A path Atlas cannot name is part of the universe it cannot account
             * for, so the walk is PARTIAL rather than silently smaller. */
            note_partial_at(w, rel_text, ATLAS_SEM_OBSTACLE_UNREPRESENTABLE,
                            "a path could not be represented within the path ceiling");
            continue;
        }
        if (atlas_sem_path_under_prefix(w->packed_excludes, child_text)) {
            w->out->excluded_subtrees++;
            /* An exclusion is a hole in the universe by the operator's own
             * instruction. It is *shown*, and it makes discovery PARTIAL rather
             * than COMPLETE: Atlas did not look there, and an operator saying
             * "do not look" is not the statement "there is nothing there". */
            note_partial_at(w, child_text, ATLAS_SEM_OBSTACLE_EXCLUDED,
                            "an operator excluded a subtree from the search");
            continue;
        }

        if (strcmp(e->d_name, COMPDB_NAME) == 0) {
            atlas_sem_input *in = intern_path(w, child_text);
            if (in != NULL) {
                mark_origin(in, ATLAS_SEM_INPUT_DISCOVERED);
                if (!in->accepted && in->reject_reason[0] == '\0') {
                    validate(w, dir_fd, e->d_name, in);
                }
            }
            continue;
        }

        /* Anything that is not a directory is not descended into, and a symlink
         * is never a directory for this purpose. `fstatat` with
         * `AT_SYMLINK_NOFOLLOW` is the house pattern — `d_type` is not filled in
         * by every filesystem, and a walk that trusted it would silently stop
         * descending on the ones that do not. */
        struct stat sb;
        memset(&sb, 0, sizeof sb);
        if (fstatat(dir_fd, e->d_name, &sb, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(sb.st_mode)) {
            continue;
        }
        if (sub_count >= WALK_MAX_SUBDIRS ||
            snprintf(names[sub_count], WALK_MAX_NAME, "%s", e->d_name) >= WALK_MAX_NAME) {
            sub_overflow = true;
            continue;
        }
        sub_count++;
    }
    (void)closedir(d);

    if (sub_overflow) {
        note_partial_at(w, rel_text, ATLAS_SEM_OBSTACLE_ENTRIES,
                        "a directory held more subdirectories than one walk level may hold");
    }

    for (size_t i = 0; i < sub_count; i++) {
        if (w->stop) {
            break;
        }
        descend(w, dir_fd, names[i], rel, rel_len, rel_text, depth);
    }
    free(names);
}

/* --- ordering ---------------------------------------------------------------- */

/* A9.2.5. Obstacles are sorted by path so two walks over an unchanged tree
 * produce the same list whatever order `readdir` returned entries in. The same
 * argument `cmp_input` makes below: a value an operator compares between runs
 * must not depend on the filesystem's iteration order. */
static int cmp_obstacle(const void *a, const void *b) {
    const atlas_sem_obstacle *x = a;
    const atlas_sem_obstacle *y = b;
    int c = strcmp(x->path, y->path);
    return c != 0 ? c : strcmp(x->reason, y->reason);
}

static int cmp_input(const void *a, const void *b) {
    const atlas_sem_input *x = a;
    const atlas_sem_input *y = b;
    return strcmp(x->path, y->path);
}

/* --- the entry point ---------------------------------------------------------- */

atlas_status atlas_sem_discover(const char *root, const atlas_sem_config *cfg,
                                void (*yield)(void *ud), void *yield_ud,
                                atlas_sem_discovery_result *out, atlas_err *err) {
    atlas_sem_discovery_result_init(out);
    if (root == NULL || out == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "build-input discovery: bad request");
    }
    atlas_now_iso8601(out->discovered_at, sizeof out->discovered_at);

    walk_state w;
    memset(&w, 0, sizeof w);
    w.out = out;
    w.root = root;
    w.yield = yield;
    w.yield_ud = yield_ud;
    w.packed_excludes =
        (cfg != NULL && cfg->excludes.len > 0) ? atlas_buf_cstr(&cfg->excludes) : "";
    out->mode = cfg != NULL ? cfg->discovery_mode : ATLAS_SEM_DISCMODE_AUTOMATIC;

    /* The registered root, opened without following a link on its final
     * component: a root replaced by a symlink since registration refuses the
     * walk rather than being followed somewhere else. */
    int root_fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root_fd < 0) {
        /* Atlas could not look at all. The result stays UNKNOWN — never an empty
         * COMPLETE, because "I could not look" must never read as "there is
         * nothing there". */
        return atlas_err_set_errno(err, ATLAS_ERR_REPO, errno,
                                   "cannot open the registered repository root for build-input "
                                   "discovery");
    }

    /* The walk first, so that a discovered path already carries its identity
     * when a pinned path naming the same file is considered — which is what
     * makes the duplicate detection order-independent. */
    atlas_status st = ATLAS_OK;
    if (out->mode == ATLAS_SEM_DISCMODE_AUTOMATIC) {
        walk_dir(&w, root_fd, "", 0, "", 0);
        if (out->state != ATLAS_SEM_DISC_PARTIAL) {
            /* The walk covered the whole bounded universe. COMPLETE is asserted
             * here, deliberately and last — A6's discipline that a permissive
             * verdict is never what a `memset` left. */
            out->state = ATLAS_SEM_DISC_COMPLETE;
        } else {
            out->state = ATLAS_SEM_DISC_PARTIAL;
        }
    }
    /* MANUAL leaves UNKNOWN. See the header: a pinned list is a list somebody
     * wrote, and this season exists because one was incomplete. */

    if (cfg != NULL && cfg->compdbs.len > 0) {
        atlas_buf list = ATLAS_BUF_INIT;
        st = atlas_sem_config_unpack(atlas_buf_cstr(&cfg->compdbs), &list, NULL, err);
        if (st == ATLAS_OK && list.data != NULL) {
            const char *p = (const char *)list.data;
            const char *end = p + list.len;
            while (p < end && *p != '\0') {
                take_pinned(&w, root_fd, p);
                p += strlen(p) + 1;
            }
        }
        atlas_buf_free(&list);
    }

    (void)close(root_fd);
    if (st != ATLAS_OK) {
        return st;
    }

    if (out->accepted_count > (size_t)ATLAS_SEM_MAX_COMPDBS) {
        /* More were accepted than one generation may hold. The excess is
         * *reported* as rejected with a reason rather than trimmed away, so the
         * result never looks like a repository that simply has fewer. */
        size_t kept = 0;
        for (size_t i = 0; i < out->count; i++) {
            if (!out->inputs[i].accepted) {
                continue;
            }
            if (kept < (size_t)ATLAS_SEM_MAX_COMPDBS) {
                kept++;
                continue;
            }
            out->inputs[i].accepted = false;
            reject(&out->inputs[i], ATLAS_SEM_REJECT_TOO_MANY);
        }
        out->accepted_count = kept;
        note_partial(&w, "more compilation databases were found than one generation may hold");
        out->state = ATLAS_SEM_DISC_PARTIAL;
    }

    /* Deterministic order, by the reported path, so two runs over one repository
     * produce the same list and the same identity whatever `readdir` did. */
    if (out->count > 1) {
        qsort(out->inputs, out->count, sizeof out->inputs[0], cmp_input);
    }
    if (out->obstacle_count > 1) {
        qsort(out->obstacles, out->obstacle_count, sizeof out->obstacles[0], cmp_obstacle);
    }
    return ATLAS_OK;
}

/* Atlas - A12.1: the reconciled-memory vocabularies, and the policy value
 * grammar that registers a memory source.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See atlas/memory.h for what the vocabularies are and for the three house
 * rules that govern all five of them.
 *
 * This file is pure. No database handle, no process, no file, no clock, and no
 * allocation -- an enum in and a name out, or a byte range in and a verdict out.
 * That is what lets the whole grammar be enumerated by a unit test: a value
 * parser reachable only through a root-owned file on disk could not be
 * exercised at all by a process that is not root, and an unexercised grammar in
 * a security-relevant file is the one thing this layer must not have.
 *
 * Every switch below has no `default:`. Adding a member to one of these
 * vocabularies must fail the build here, and at every other site that has to
 * decide about it, rather than falling into a branch that happens to compile.
 */
#define _GNU_SOURCE 1

#include "atlas/memory.h"

#include <string.h>

/* For the complete `atlas_syspolicy_memory_source`, which `memory.h` declares
 * incomplete on purpose so the dependency between the two headers runs one way.
 * The value parser lives here rather than in `src/core/syspolicy.c` so the
 * grammar can be enumerated by a unit test; the loader calls it. */
#include "atlas/syspolicy.h"

/* --- what a memory source is, and who reads it ----------------------------- */

const char *atlas_memory_source_class_name(atlas_memory_source_class c) {
    switch (c) {
    case ATLAS_MEMORY_SOURCE_REPO_FILE: return "REPO_FILE";
    case ATLAS_MEMORY_SOURCE_REPO_DIR: return "REPO_DIR";
    case ATLAS_MEMORY_SOURCE_EXTERNAL_FILE: return "EXTERNAL_FILE";
    case ATLAS_MEMORY_SOURCE_EXTERNAL_DIR: return "EXTERNAL_DIR";
    case ATLAS_MEMORY_SOURCE_UNKNOWN: break;
    }
    return "UNKNOWN";
}

bool atlas_memory_source_class_parse(const char *name, atlas_memory_source_class *out) {
    if (name == NULL) {
        return false;
    }
    /* UNKNOWN deliberately does not parse; see the header. Exact spelling only,
     * with no lowercase alternative: this grammar is read out of a root-owned
     * policy file, and a parser that accepts two spellings of one member is a
     * parser whose author and reader can disagree about what the file said. */
    if (strcmp(name, "REPO_FILE") == 0) {
        *out = ATLAS_MEMORY_SOURCE_REPO_FILE;
        return true;
    }
    if (strcmp(name, "REPO_DIR") == 0) {
        *out = ATLAS_MEMORY_SOURCE_REPO_DIR;
        return true;
    }
    if (strcmp(name, "EXTERNAL_FILE") == 0) {
        *out = ATLAS_MEMORY_SOURCE_EXTERNAL_FILE;
        return true;
    }
    if (strcmp(name, "EXTERNAL_DIR") == 0) {
        *out = ATLAS_MEMORY_SOURCE_EXTERNAL_DIR;
        return true;
    }
    return false;
}

bool atlas_memory_source_class_is_repo(atlas_memory_source_class c) {
    switch (c) {
    case ATLAS_MEMORY_SOURCE_REPO_FILE:
    case ATLAS_MEMORY_SOURCE_REPO_DIR:
        return true;
    case ATLAS_MEMORY_SOURCE_EXTERNAL_FILE:
    case ATLAS_MEMORY_SOURCE_EXTERNAL_DIR:
    case ATLAS_MEMORY_SOURCE_UNKNOWN:
        break;
    }
    /* A zero is not a repository class. It is not any class, and answering
     * "true" here would hand an unfilled struct the daemon's own read path. */
    return false;
}

/* --- what a pass found had happened ---------------------------------------- */

const char *atlas_memory_diff_kind_name(atlas_memory_diff_kind k) {
    switch (k) {
    case ATLAS_MEMORY_DIFF_ADDED: return "ADDED";
    case ATLAS_MEMORY_DIFF_CHANGED: return "CHANGED";
    case ATLAS_MEMORY_DIFF_SUPPORTED: return "SUPPORTED";
    case ATLAS_MEMORY_DIFF_CONTRADICTED: return "CONTRADICTED";
    case ATLAS_MEMORY_DIFF_STALE: return "STALE";
    case ATLAS_MEMORY_DIFF_IMPACTED: return "IMPACTED";
    case ATLAS_MEMORY_DIFF_SUPERSEDED: return "SUPERSEDED";
    case ATLAS_MEMORY_DIFF_UNDETERMINED: return "UNDETERMINED";
    case ATLAS_MEMORY_DIFF_UNKNOWN: break;
    }
    return "UNKNOWN";
}

bool atlas_memory_diff_kind_parse(const char *name, atlas_memory_diff_kind *out) {
    if (name == NULL) {
        return false;
    }
    if (strcmp(name, "ADDED") == 0) {
        *out = ATLAS_MEMORY_DIFF_ADDED;
        return true;
    }
    if (strcmp(name, "CHANGED") == 0) {
        *out = ATLAS_MEMORY_DIFF_CHANGED;
        return true;
    }
    if (strcmp(name, "SUPPORTED") == 0) {
        *out = ATLAS_MEMORY_DIFF_SUPPORTED;
        return true;
    }
    if (strcmp(name, "CONTRADICTED") == 0) {
        *out = ATLAS_MEMORY_DIFF_CONTRADICTED;
        return true;
    }
    if (strcmp(name, "STALE") == 0) {
        *out = ATLAS_MEMORY_DIFF_STALE;
        return true;
    }
    if (strcmp(name, "IMPACTED") == 0) {
        *out = ATLAS_MEMORY_DIFF_IMPACTED;
        return true;
    }
    if (strcmp(name, "SUPERSEDED") == 0) {
        *out = ATLAS_MEMORY_DIFF_SUPERSEDED;
        return true;
    }
    if (strcmp(name, "UNDETERMINED") == 0) {
        *out = ATLAS_MEMORY_DIFF_UNDETERMINED;
        return true;
    }
    return false;
}

/* --- what a proposition is anchored to ------------------------------------- */

const char *atlas_memory_anchor_kind_name(atlas_memory_anchor_kind k) {
    switch (k) {
    case ATLAS_MEMORY_ANCHOR_PATH: return "PATH";
    case ATLAS_MEMORY_ANCHOR_SYMBOL: return "SYMBOL";
    case ATLAS_MEMORY_ANCHOR_DECISION: return "DECISION";
    case ATLAS_MEMORY_ANCHOR_COMMIT: return "COMMIT";
    case ATLAS_MEMORY_ANCHOR_UNKNOWN: break;
    }
    return "UNKNOWN";
}

bool atlas_memory_anchor_kind_parse(const char *name, atlas_memory_anchor_kind *out) {
    if (name == NULL) {
        return false;
    }
    if (strcmp(name, "PATH") == 0) {
        *out = ATLAS_MEMORY_ANCHOR_PATH;
        return true;
    }
    if (strcmp(name, "SYMBOL") == 0) {
        *out = ATLAS_MEMORY_ANCHOR_SYMBOL;
        return true;
    }
    if (strcmp(name, "DECISION") == 0) {
        *out = ATLAS_MEMORY_ANCHOR_DECISION;
        return true;
    }
    if (strcmp(name, "COMMIT") == 0) {
        *out = ATLAS_MEMORY_ANCHOR_COMMIT;
        return true;
    }
    return false;
}

/* --- whether a frozen pack still describes the world ----------------------- */

const char *atlas_memory_pack_status_name(atlas_memory_pack_status s) {
    switch (s) {
    case ATLAS_MEMORY_PACK_CURRENT: return "CURRENT";
    case ATLAS_MEMORY_PACK_STALE: return "STALE";
    case ATLAS_MEMORY_PACK_UNKNOWN: break;
    }
    return "UNKNOWN";
}

bool atlas_memory_pack_status_parse(const char *name, atlas_memory_pack_status *out) {
    if (name == NULL) {
        return false;
    }
    if (strcmp(name, "CURRENT") == 0) {
        *out = ATLAS_MEMORY_PACK_CURRENT;
        return true;
    }
    if (strcmp(name, "STALE") == 0) {
        *out = ATLAS_MEMORY_PACK_STALE;
        return true;
    }
    return false;
}

/* --- why a generation was produced ----------------------------------------- */

const char *atlas_memory_gen_cause_name(atlas_memory_gen_cause c) {
    switch (c) {
    case ATLAS_MEMORY_CAUSE_SOURCE_REVISION: return "SOURCE_REVISION";
    case ATLAS_MEMORY_CAUSE_DECISION_REVISION: return "DECISION_REVISION";
    case ATLAS_MEMORY_CAUSE_COMMIT: return "COMMIT";
    case ATLAS_MEMORY_CAUSE_UNKNOWN: break;
    }
    return "UNKNOWN";
}

bool atlas_memory_gen_cause_parse(const char *name, atlas_memory_gen_cause *out) {
    if (name == NULL) {
        return false;
    }
    if (strcmp(name, "SOURCE_REVISION") == 0) {
        *out = ATLAS_MEMORY_CAUSE_SOURCE_REVISION;
        return true;
    }
    if (strcmp(name, "DECISION_REVISION") == 0) {
        *out = ATLAS_MEMORY_CAUSE_DECISION_REVISION;
        return true;
    }
    if (strcmp(name, "COMMIT") == 0) {
        *out = ATLAS_MEMORY_CAUSE_COMMIT;
        return true;
    }
    return false;
}

/* --- the policy value grammar ----------------------------------------------
 *
 * `CLASS[@repository]:path`. See atlas/memory.h for what the form means and for
 * why this function is exposed rather than being a static inside the loader.
 *
 * Deliberately as dull as the policy parser that calls it: no quoting, no
 * escapes, no expansion, no normalisation, and no repair. Every one of those
 * would be a parser feature whose bugs are reachable from a security-relevant
 * file, and none of them buys an operator anything a second line would not.
 */

/* One component of a path, tested by name rather than by substring.
 *
 * `..` is a component that leaves the tree it is written relative to; `a..b.md`
 * is an ordinary filename that merely contains two dots. A `strstr` cannot tell
 * them apart, which is why this walks components instead. */
static bool path_component_is(const char *s, size_t start, size_t end, const char *want) {
    size_t n = strlen(want);
    return end - start == n && memcmp(s + start, want, n) == 0;
}

/* Refuses a `..` component anywhere, and a `.git` component when the path is
 * repository-relative.
 *
 * `..` is refused in an external path too, for the reason `plausible_abs_path`
 * in `src/core/syspolicy.c` refuses `/../` in the socket path and the data
 * directory: a path an operator has to resolve in their head before they can say
 * which file it names is one they cannot read back. `.git` is refused only for a
 * REPO_ class, where the reason is specific — the repository's own metadata is
 * not a memory source, and those bytes belong to the git safety layer rather
 * than to a prose extractor. An external path has no repository to have metadata
 * of, so the rule would be a rule about nothing. */
static bool path_components_are_safe(const char *p, size_t len, bool repo_relative) {
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && p[i] != '/') {
            i++;
        }
        size_t end = i;
        if (i < len) {
            i++;
        }
        if (path_component_is(p, start, end, "..")) {
            return false;
        }
        if (repo_relative && path_component_is(p, start, end, ".git")) {
            return false;
        }
    }
    return true;
}

bool atlas_memory_source_value_parse(const char *val, size_t len,
                                     atlas_syspolicy_memory_source *out) {
    if (out == NULL) {
        return false;
    }
    /* Zeroed first, so every refusal below leaves UNKNOWN rather than whatever
     * the caller's stack held -- and so a caller that ignored the return value
     * is holding a class that asserts nothing. */
    memset(out, 0, sizeof(*out));
    if (val == NULL || len == 0) {
        return false;
    }
    /* A NUL inside the value would silently shorten whatever was stored to the
     * NUL, which is the one failure this grammar exists to avoid. It cannot come
     * from a well-formed policy and it is refused rather than trusted not to. */
    if (memchr(val, '\0', len) != NULL) {
        return false;
    }

    const char *colon = memchr(val, ':', len);
    if (colon == NULL) {
        return false;
    }
    size_t head_len = (size_t)(colon - val);
    const char *path = colon + 1;
    size_t path_len = len - head_len - 1u;

    /* The head is `CLASS` or `CLASS@repository`, split at the *first* `@`. */
    const char *at = memchr(val, '@', head_len);
    size_t class_len = at != NULL ? (size_t)(at - val) : head_len;
    const char *repo = at != NULL ? at + 1 : NULL;
    size_t repo_len = at != NULL ? head_len - class_len - 1u : 0;

    /* The class, through the vocabulary's own parser rather than a second
     * comparison here: one spelling of one member, in one place. */
    char class_name[32];
    if (class_len == 0 || class_len + 1u > sizeof class_name) {
        return false;
    }
    memcpy(class_name, val, class_len);
    class_name[class_len] = '\0';
    atlas_memory_source_class cls = ATLAS_MEMORY_SOURCE_UNKNOWN;
    if (!atlas_memory_source_class_parse(class_name, &cls)) {
        return false;
    }

    /* The repository name, when one is given. A registry name, never a path: a
     * `/` in it would make it name a place instead of a repository, and `.` or
     * `..` are path components that no registered repository is called. Refused
     * rather than truncated when it does not fit. */
    if (at != NULL) {
        if (repo_len == 0 || repo_len + 1u > sizeof out->repo_name) {
            return false;
        }
        if (memchr(repo, '/', repo_len) != NULL) {
            return false;
        }
        if (path_component_is(repo, 0, repo_len, ".") ||
            path_component_is(repo, 0, repo_len, "..")) {
            return false;
        }
        memcpy(out->repo_name, repo, repo_len);
        out->repo_name[repo_len] = '\0';
    }

    /* The path. Which shape it must have is decided by the class rather than
     * inferred from the bytes, so the two can never disagree about what is being
     * named -- and `atlas_memory_source_class_is_repo` is the one implementation
     * of that question. */
    if (path_len == 0 || path_len + 1u > sizeof out->path) {
        return false;
    }
    bool repo_relative = atlas_memory_source_class_is_repo(cls);
    if (repo_relative == (path[0] == '/')) {
        /* A repository path that is absolute, or an external path that is not. */
        return false;
    }
    if (!path_components_are_safe(path, path_len, repo_relative)) {
        return false;
    }
    memcpy(out->path, path, path_len);
    out->path[path_len] = '\0';

    out->cls = cls;
    return true;
}

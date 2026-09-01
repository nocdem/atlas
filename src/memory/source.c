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

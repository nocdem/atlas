/* Atlas - A9.2: loading the root-owned verification policy.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See atlas/verifypolicy.h for what a policy establishes and what it does not.
 *
 * The whole file fails closed. There is one `out->state =
 * ATLAS_VERIFYPOLICY_ENABLED` assignment, it is the last statement of the
 * parser, and every path that does not reach it leaves the struct as `memset`
 * left it — which automates nothing, because nothing is zero.
 */
#define _GNU_SOURCE 1

#include "atlas/verifypolicy.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "atlas/rootpath.h"

/* Bounded for the reason every other Atlas policy is: a file Atlas parses is a
 * file whose size is somebody else's choice — root's here — and refusing an
 * oversized one costs nothing. Refused rather than truncated, because a
 * truncated policy is a *different* policy and the part lost is whichever line
 * happened to be last. */
#define VERIFYPOLICY_MAX_BYTES 16384u

const char *atlas_verifypolicy_state_name(atlas_verifypolicy_state s) {
    return s == ATLAS_VERIFYPOLICY_ENABLED ? "ENABLED" : "DISABLED";
}

const char *atlas_verifypolicy_reason_name(atlas_verifypolicy_reason r) {
    switch (r) {
    case ATLAS_VERIFYPOLICY_REASON_UNKNOWN: return "UNKNOWN";
    case ATLAS_VERIFYPOLICY_REASON_ABSENT: return "ABSENT";
    case ATLAS_VERIFYPOLICY_REASON_PATH_UNSAFE: return "PATH_UNSAFE";
    case ATLAS_VERIFYPOLICY_REASON_WRITABLE: return "WRITABLE";
    case ATLAS_VERIFYPOLICY_REASON_MALFORMED: return "MALFORMED";
    case ATLAS_VERIFYPOLICY_REASON_DISABLED: return "DISABLED";
    case ATLAS_VERIFYPOLICY_REASON_ACTIVE: return "ACTIVE";
    }
    return "UNKNOWN";
}

const char *atlas_verifypolicy_reason_detail(atlas_verifypolicy_reason r) {
    switch (r) {
    case ATLAS_VERIFYPOLICY_REASON_UNKNOWN:
        return "the verification policy was never loaded, so Atlas changes no lifecycle state on "
               "its own";
    case ATLAS_VERIFYPOLICY_REASON_ABSENT:
        return "no verification policy is installed at " ATLAS_VERIFYPOLICY_PATH ", so every "
               "lifecycle transition requires the operator channel";
    case ATLAS_VERIFYPOLICY_REASON_PATH_UNSAFE:
        return "a component of the policy path is a symbolic link or is malformed, so whoever can "
               "create links there would choose what Atlas may automate";
    case ATLAS_VERIFYPOLICY_REASON_WRITABLE:
        return "the policy, or a directory leading to it, can be modified by somebody other than "
               "root, so it constrains nobody — least of all the engine it is meant to bound";
    case ATLAS_VERIFYPOLICY_REASON_MALFORMED:
        return "the policy exists but does not describe a complete, safe set of automatic "
               "transitions";
    case ATLAS_VERIFYPOLICY_REASON_DISABLED:
        return "the policy is installed and says `enabled = no`, so nothing is automatic";
    case ATLAS_VERIFYPOLICY_REASON_ACTIVE:
        return "a root-anchored policy names which transitions Atlas may make on its own, and "
               "under which verifier";
    }
    return "nothing is automatic";
}

/* --- transitions no policy may authorise ----------------------------------
 *
 * Asked before the file is consulted, so that an operator's mistake is refused
 * rather than obeyed. "Root wrote it" establishes that the instruction is
 * authentic, not that it is one Atlas should carry out. */
bool atlas_verifypolicy_transition_forbidden(atlas_decision_kind kind, atlas_decision_state from,
                                             atlas_decision_state to, atlas_verify_reason *why) {
    /* Risk acceptance. Atlas can establish that a risk is real and that it has
     * been mitigated; that the project is willing to live with it is a
     * different sentence with an owner, and no quantity of evidence supplies
     * one. */
    if (kind == ATLAS_DECISION_KIND_ACCEPTED_RISK && to == ATLAS_DECISION_APPROVED) {
        if (why != NULL) {
            *why = ATLAS_VREASON_RISK_REQUIRES_AUTHORITY;
        }
        return true;
    }
    /* Rejection. §34: auto-reject needs an explicit policy-defined
     * falsification condition, and low confidence is not one — it usually means
     * missing evidence, an index that has not run, or a scope mismatch. Absent
     * rather than merely refused: there is no rule syntax that names REJECTED
     * as a target, so this branch is the second layer rather than the only
     * one. */
    if (to == ATLAS_DECISION_REJECTED) {
        if (why != NULL) {
            *why = ATLAS_VREASON_NOT_ALLOWED;
        }
        return true;
    }
    /* The state machine has the final say on legality, and it is kind-aware.
     * Asked here as well so a policy naming an impossible transition is
     * reported when it is used rather than silently never matching. */
    if (!atlas_decision_transition_allowed(kind, from, to)) {
        if (why != NULL) {
            *why = ATLAS_VREASON_TRANSITION_ILLEGAL;
        }
        return true;
    }
    return false;
}

const atlas_verifypolicy_rule *atlas_verifypolicy_find(const atlas_verifypolicy *p,
                                                       atlas_decision_kind kind,
                                                       atlas_decision_state from,
                                                       atlas_decision_state to) {
    if (p == NULL || p->state != ATLAS_VERIFYPOLICY_ENABLED) {
        return NULL;
    }
    for (size_t i = 0; i < p->rule_count; i++) {
        const atlas_verifypolicy_rule *r = &p->rules[i];
        if (r->kind == kind && r->from == from && r->to == to) {
            return r;
        }
    }
    return NULL;
}

/* --- parsing --------------------------------------------------------------- */

static bool parse_bool(const char *v, bool *out) {
    if (strcmp(v, "yes") == 0 || strcmp(v, "true") == 0 || strcmp(v, "1") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(v, "no") == 0 || strcmp(v, "false") == 0 || strcmp(v, "0") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_int(const char *v, long long lo, long long hi, long long *out) {
    if (v[0] == '\0') {
        return false;
    }
    char *end = NULL;
    errno = 0;
    long long n = strtoll(v, &end, 10);
    if (errno != 0 || end == NULL || *end != '\0' || n < lo || n > hi) {
        return false;
    }
    *out = n;
    return true;
}

/* `allow = KIND FROM TO VERIFIER`, four whitespace-separated tokens.
 *
 * Four and exactly four. A rule with a missing verifier is refused rather than
 * defaulted, because a deterministic rule with no named verifier is a rule any
 * evidence at all satisfies — which is the single most dangerous line this file
 * could contain and therefore the one an absent token must never produce. */
static bool parse_rule(const char *v, atlas_verifypolicy_rule *out) {
    char tok[4][64];
    size_t ntok = 0;
    size_t i = 0;
    while (v[i] != '\0' && ntok < 4) {
        while (v[i] == ' ' || v[i] == '\t') {
            i++;
        }
        if (v[i] == '\0') {
            break;
        }
        size_t n = 0;
        while (v[i] != '\0' && v[i] != ' ' && v[i] != '\t' && n + 1 < sizeof tok[0]) {
            tok[ntok][n++] = v[i++];
        }
        /* A token longer than the buffer is malformed, not truncated: a
         * truncated verifier name could parse as a different verifier. */
        if (v[i] != '\0' && v[i] != ' ' && v[i] != '\t') {
            return false;
        }
        tok[ntok][n] = '\0';
        ntok++;
    }
    while (v[i] == ' ' || v[i] == '\t') {
        i++;
    }
    if (ntok != 4 || v[i] != '\0') {
        return false;
    }

    if (!atlas_decision_kind_parse(tok[0], &out->kind)) {
        return false;
    }
    if (!atlas_decision_state_parse(tok[1], &out->from)) {
        return false;
    }
    if (!atlas_decision_state_parse(tok[2], &out->to)) {
        return false;
    }
    if (!atlas_verify_verifier_parse(tok[3], &out->verifier)) {
        return false;
    }
    if (out->verifier == ATLAS_VERIFIER_NONE) {
        return false;
    }
    /* A rule naming a transition Atlas would refuse anyway is malformed rather
     * than inert. An inert rule reads, to whoever wrote it, exactly like one
     * that works. */
    if (atlas_verifypolicy_transition_forbidden(out->kind, out->from, out->to, NULL)) {
        return false;
    }
    return true;
}

void atlas_verifypolicy_parse_buffer(const char *buf, size_t total, atlas_verifypolicy *out) {
    memset(out, 0, sizeof(*out));
    out->state = ATLAS_VERIFYPOLICY_DISABLED;
    out->reason = ATLAS_VERIFYPOLICY_REASON_MALFORMED;

    /* The hash is over the exact bytes, before any interpretation, so an audit
     * row identifies the file rather than Atlas' reading of it. */
    atlas_sha256_hex(buf, total, out->policy_hash);

    /* Defaults, set before parsing so an absent key means the documented
     * default rather than a zero that happens to be safe by accident. Every one
     * of them is at its most demanding: absent keys never loosen anything. */
    (void)snprintf(out->policy_id, sizeof out->policy_id, "unnamed-verification-policy");
    out->deterministic_enforce = false;
    out->empirical_enforce = false;
    out->min_confidence = 100;
    out->min_evidence_groups = 1;
    out->max_evidence_age = ATLAS_VERIFY_DEFAULT_MAX_EVIDENCE_AGE;
    out->min_calibration_samples = 100;

    bool have_enabled = false;
    bool enabled = false;

    size_t i = 0;
    while (i < total) {
        size_t start = i;
        while (i < total && buf[i] != '\n') {
            i++;
        }
        size_t end = i;
        if (i < total) {
            i++;
        }
        while (start < end && (buf[start] == ' ' || buf[start] == '\t')) {
            start++;
        }
        while (end > start && (buf[end - 1] == ' ' || buf[end - 1] == '\t' ||
                               buf[end - 1] == '\r')) {
            end--;
        }
        if (start == end || buf[start] == '#') {
            continue;
        }

        size_t linelen = end - start;
        if (linelen >= 512) {
            return; /* malformed */
        }
        char line[512];
        memcpy(line, buf + start, linelen);
        line[linelen] = '\0';

        char *eq = strchr(line, '=');
        if (eq == NULL) {
            return; /* not a key = value line */
        }
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        /* Trim around the separator. */
        size_t klen = strlen(key);
        while (klen > 0 && (key[klen - 1] == ' ' || key[klen - 1] == '\t')) {
            key[--klen] = '\0';
        }
        while (*val == ' ' || *val == '\t') {
            val++;
        }

        long long n = 0;
        if (strcmp(key, "enabled") == 0) {
            if (!parse_bool(val, &enabled)) {
                return;
            }
            have_enabled = true;
        } else if (strcmp(key, "policy_id") == 0) {
            if (val[0] == '\0' || strlen(val) >= sizeof out->policy_id) {
                return;
            }
            (void)snprintf(out->policy_id, sizeof out->policy_id, "%s", val);
        } else if (strcmp(key, "deterministic_enforce") == 0) {
            if (!parse_bool(val, &out->deterministic_enforce)) {
                return;
            }
        } else if (strcmp(key, "empirical_enforce") == 0) {
            if (!parse_bool(val, &out->empirical_enforce)) {
                return;
            }
        } else if (strcmp(key, "min_confidence") == 0) {
            if (!parse_int(val, 0, 100, &n)) {
                return;
            }
            out->min_confidence = (int)n;
        } else if (strcmp(key, "min_evidence_groups") == 0) {
            if (!parse_int(val, 1, 64, &n)) {
                return;
            }
            out->min_evidence_groups = (int)n;
        } else if (strcmp(key, "max_evidence_age") == 0) {
            if (!parse_int(val, 1, 31536000, &n)) {
                return;
            }
            out->max_evidence_age = n;
        } else if (strcmp(key, "min_calibration_samples") == 0) {
            if (!parse_int(val, 1, 1000000, &n)) {
                return;
            }
            out->min_calibration_samples = (int)n;
        } else if (strcmp(key, "allow") == 0) {
            if (out->rule_count >= ATLAS_VERIFY_MAX_POLICY_RULES) {
                /* Refused, never dropped. A policy with more rules than Atlas
                 * keeps is one whose author configured something that was
                 * never read — and here that something authorises automatic
                 * changes to project knowledge. */
                return;
            }
            atlas_verifypolicy_rule r;
            memset(&r, 0, sizeof r);
            if (!parse_rule(val, &r)) {
                return;
            }
            out->rules[out->rule_count++] = r;
        } else {
            /* A7.1's rule. An unrecognised key is an error, because the thing
             * its author most plausibly believes they configured is a
             * restriction. */
            return;
        }
    }

    if (!have_enabled) {
        return;
    }
    if (!enabled) {
        out->reason = ATLAS_VERIFYPOLICY_REASON_DISABLED;
        return;
    }
    /* Enforcement with no rules is a contradiction an operator should be told
     * about rather than a quiet no-op: it reads, to whoever wrote it, as though
     * automation is on. */
    if ((out->deterministic_enforce || out->empirical_enforce) && out->rule_count == 0) {
        return;
    }

    out->reason = ATLAS_VERIFYPOLICY_REASON_ACTIVE;
    out->state = ATLAS_VERIFYPOLICY_ENABLED;
}

void atlas_verifypolicy_load_at(const char *path, atlas_verifypolicy *out) {
    memset(out, 0, sizeof(*out));
    out->state = ATLAS_VERIFYPOLICY_DISABLED;
    out->reason = ATLAS_VERIFYPOLICY_REASON_ABSENT;

    atlas_rootpath_result rr = ATLAS_ROOTPATH_UNKNOWN;
    int fd = atlas_rootpath_open(path, false, &rr, out->detail, sizeof(out->detail));
    if (fd < 0) {
        switch (rr) {
        case ATLAS_ROOTPATH_MISSING:
            out->reason = ATLAS_VERIFYPOLICY_REASON_ABSENT;
            break;
        case ATLAS_ROOTPATH_SYMLINK:
        case ATLAS_ROOTPATH_BAD_PATH:
            out->reason = ATLAS_VERIFYPOLICY_REASON_PATH_UNSAFE;
            break;
        case ATLAS_ROOTPATH_NOT_REGULAR:
        case ATLAS_ROOTPATH_NOT_DIRECTORY:
            out->reason = ATLAS_VERIFYPOLICY_REASON_MALFORMED;
            break;
        case ATLAS_ROOTPATH_UNKNOWN:
        case ATLAS_ROOTPATH_OK:
        case ATLAS_ROOTPATH_WRITABLE:
            out->reason = ATLAS_VERIFYPOLICY_REASON_WRITABLE;
            break;
        }
        return;
    }

    char buf[VERIFYPOLICY_MAX_BYTES];
    size_t total = 0;
    while (total < sizeof(buf)) {
        ssize_t got = read(fd, buf + total, sizeof(buf) - total);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            (void)close(fd);
            out->reason = ATLAS_VERIFYPOLICY_REASON_MALFORMED;
            return;
        }
        if (got == 0) {
            break;
        }
        total += (size_t)got;
    }
    (void)close(fd);
    if (total == sizeof(buf)) {
        out->reason = ATLAS_VERIFYPOLICY_REASON_MALFORMED;
        return;
    }

    char detail[512];
    (void)snprintf(detail, sizeof detail, "%s", out->detail);
    atlas_verifypolicy_parse_buffer(buf, total, out);
    (void)snprintf(out->detail, sizeof out->detail, "%s", detail);
}

void atlas_verifypolicy_load(atlas_verifypolicy *out) {
    atlas_verifypolicy_load_at(ATLAS_VERIFYPOLICY_PATH, out);
}

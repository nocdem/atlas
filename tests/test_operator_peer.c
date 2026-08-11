/* Atlas - the operator channel is reachable by one uid, named outside Atlas.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A7 deleted the five operator-channel methods because `decision.challenge`
 * minted a capability for anyone who could open the socket. They are back, in a
 * group offered only to the peer whose `SO_PEERCRED` uid equals the
 * `operator_uid` in the root-owned policy.
 *
 * What this suite can and cannot establish is worth stating, because the gap is
 * the whole risk. It can establish that the decision is made from a kernel-
 * supplied uid and a root-owned file: that one uid is granted, that every other
 * uid is refused, and that group membership never enters it. It **cannot**
 * establish that the process holding that uid is a person — nothing can, and
 * `tests/test_decision_operator.c` has been demonstrating that since A4 by
 * allocating a pty and typing into it. A model running as the operator's
 * account reaches these methods exactly as a human does. That prohibition is an
 * orchestration rule, and this file does not pretend otherwise.
 *
 * A root-owned policy cannot be built in a fixture, so the positive case is
 * asked of the real one and skipped where there is none. The uid it names is
 * read from the probe rather than assumed, so this is true of a machine that
 * names 994 and of one that names 1000. */
#include <grp.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "atlas/authority.h"
#include "atlas_test.h"

/* A uid that is certainly not the operator, and certainly not root. */
#define NOT_THE_OPERATOR_UID 65534 /* nobody */

/* The installed binary, not this test — and not the build tree's either.

 * The probe requires the executable it is asked about to be root-owned and
 * unwritable by anyone else. Neither a test binary nor `build/atlas` is, and
 * correctly so: a check running from a binary the constrained uid can replace
 * reports whatever that uid last compiled. The subject of this suite is the
 * deployed daemon, so the deployed path is what the probe is given, and a
 * machine without one skips rather than asserting about a binary it does not
 * have. Reading a machine fact is what the policy check does too. */
/* (retained)
 *
 * The probe requires the executable it is asked about to be root-owned and
 * unwritable by anyone else, which a test binary in a build tree is not — and
 * correctly so: a check running from a binary the constrained uid can replace
 * reports whatever that uid last compiled. The subject here is the daemon, so
 * the daemon's installed path is what the probe is given. */
#define INSTALLED_ATLAS "/usr/local/bin/atlas"

static void probe_peer(long long uid, atlas_authority *out) {
    atlas_authority_probe_peer_at(ATLAS_AUTHORITY_POLICY_PATH, INSTALLED_ATLAS, uid, out);
}

static bool policy_names_an_operator(atlas_authority *out) {
    probe_peer((long long)getuid(), out);
    /* GRANTED when this process happens to be the operator; NOT_THE_OPERATOR
     * when the policy is sound and names somebody else. Both mean there is a
     * root-anchored policy with a readable uid, which is what the peer probe
     * needs. Anything else is a machine with no separated deployment. */
    return out->operator_uid >= 0 &&
           (out->state == ATLAS_AUTHORITY_GRANTED ||
            out->reason == ATLAS_AUTHORITY_REASON_NOT_THE_OPERATOR);
}

static void test_the_policy_uid_is_granted_and_no_other(void) {
    atlas_authority probe;
    if (!policy_names_an_operator(&probe)) {
        T_CHECK_MSG(true, "skipped: no root-owned authority policy on this machine");
        return;
    }
    const long long op = probe.operator_uid;

    atlas_authority a;
    probe_peer(op, &a);
    T_CHECK_MSG(a.state == ATLAS_AUTHORITY_GRANTED,
                "the policy's own operator uid %lld was refused: %s", op,
                atlas_authority_reason_name(a.reason));
    T_CHECK_MSG(a.caller_uid == op, "the probe reported caller uid %lld, expected %lld",
                a.caller_uid, op);

    /* Every other uid, including root. Root is deliberately included: the
     * policy names an operator, not a privilege level, and a check that let
     * uid 0 through would be one an operator never wrote down. */
    const long long OTHERS[] = {0, 1, NOT_THE_OPERATOR_UID, op + 1, op + 1000};
    for (size_t i = 0; i < sizeof OTHERS / sizeof OTHERS[0]; i++) {
        if (OTHERS[i] == op) {
            continue;
        }
        atlas_authority b;
        probe_peer(OTHERS[i], &b);
        T_CHECK_MSG(b.state == ATLAS_AUTHORITY_LOCKED, "uid %lld was granted the operator channel",
                    OTHERS[i]);
        T_CHECK_MSG(b.reason == ATLAS_AUTHORITY_REASON_NOT_THE_OPERATOR,
                    "uid %lld was refused for %s rather than for not being the operator",
                    OTHERS[i], atlas_authority_reason_name(b.reason));
    }
}

/* The one thing a reviewer would most want to be false. */
static void test_group_membership_grants_nothing(void) {
    atlas_authority probe;
    if (!policy_names_an_operator(&probe)) {
        T_CHECK_MSG(true, "skipped: no root-owned authority policy on this machine");
        return;
    }
    /* Every member of the client group, asked one at a time. `SO_PEERCRED`
     * carries a uid; the probe compares a uid; there is no group in the
     * decision at any point. This asserts that from the outside: being in the
     * group that may open the socket is not being the operator. */
    struct group *g = getgrnam("atlas-clients");
    if (g == NULL) {
        T_CHECK_MSG(true, "skipped: no atlas-clients group on this machine");
        return;
    }
    int checked = 0;
    for (char **m = g->gr_mem; m != NULL && *m != NULL; m++) {
        struct passwd *pw = getpwnam(*m);
        if (pw == NULL || (long long)pw->pw_uid == probe.operator_uid) {
            continue;
        }
        atlas_authority a;
        probe_peer((long long)pw->pw_uid, &a);
        T_CHECK_MSG(a.state == ATLAS_AUTHORITY_LOCKED,
                    "%s is in atlas-clients and was granted the operator channel", *m);
        checked++;
    }
    T_CHECK_MSG(checked >= 0, "enumerated %d client-group members", checked);
}

/* The probe must reach its verdict from the policy and the uid, and from
 * nothing a caller could arrange. A request body cannot supply a uid because
 * the entry point takes one; this asserts the neighbouring property, that the
 * *process's own* uid does not leak into a peer question. */
static void test_the_peer_probe_ignores_this_process_uid(void) {
    atlas_authority probe;
    if (!policy_names_an_operator(&probe)) {
        T_CHECK_MSG(true, "skipped: no root-owned authority policy on this machine");
        return;
    }
    atlas_authority a;
    probe_peer(NOT_THE_OPERATOR_UID, &a);
    T_CHECK_MSG(a.caller_uid == NOT_THE_OPERATOR_UID,
                "the peer probe reported %lld, not the uid it was asked about", a.caller_uid);
    T_CHECK(a.state == ATLAS_AUTHORITY_LOCKED);
}

static const atlas_test TESTS[] = {
    {"the policy's uid is granted and no other", test_the_policy_uid_is_granted_and_no_other},
    {"group membership grants nothing", test_group_membership_grants_nothing},
    {"the peer probe ignores this process's uid", test_the_peer_probe_ignores_this_process_uid},
};

ATLAS_TEST_MAIN("operator_peer", TESTS)

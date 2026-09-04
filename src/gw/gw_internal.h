/* Atlas - A16: the gateway's test-only status entry point.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Not a public header. `include/atlas/gateway.h` is what a caller outside
 * this directory and `tests/` may use.
 *
 * P0's rule for a test channel applies here exactly as it does to the
 * writer's stall and discarded-write injections in `daemon/daemon_internal.h`:
 * a way to reach behaviour that exists only to make a test possible belongs
 * beside the code it tests, never in a public header a real caller might
 * discover and depend on. `ipc/server_internal.h` is the precedent this
 * follows, and `tests/` already has `src/` on its include path, which is what
 * `tests/test_gateway.c` uses to reach this file.
 */
#ifndef ATLAS_GW_INTERNAL_H
#define ATLAS_GW_INTERNAL_H

#include <stdbool.h>
#include <stdio.h>

#include "atlas/error.h"
#include "atlas/gwpolicy.h"

/* The same rendering `atlas_service_gateway_status` performs, over a policy
 * the caller already has rather than the compiled-in path. Split out for
 * `tests/test_gateway.c`: the loader's root-ownership walk can only succeed
 * against a genuinely root-owned file, so a test driven through the public
 * entry point could only ever render this machine's own policy, whatever it
 * happens to be today. This is the same reasoning `atlas_gwpolicy_parse_buffer`
 * exists for, one layer up -- production reaches it only through
 * `atlas_service_gateway_status`. It renders a struct the caller already
 * holds, grants nothing, opens nothing, and cannot make an untrusted policy
 * trusted, which is what keeps this a Minor risk despite living outside the
 * public header. */
atlas_status atlas_service_gateway_status_for(FILE *out, bool json, const atlas_gwpolicy *p,
                                              atlas_err *err);

#endif /* ATLAS_GW_INTERNAL_H */

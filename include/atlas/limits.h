/* Atlas - shared field limits.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * These bounds are deliberately explicit: every fixed-size field in Atlas has a
 * documented ceiling, and input that exceeds it is rejected with a clear error
 * rather than silently truncated.
 */
#ifndef ATLAS_LIMITS_H
#define ATLAS_LIMITS_H

/* User-facing repository name. */
#define ATLAS_NAME_MAX 128u
/* Hex object id: 40 for SHA-1, 64 for SHA-256. */
#define ATLAS_OID_HEX_MAX 64u
#define ATLAS_OID_HEX_MAX_INCL (ATLAS_OID_HEX_MAX + 1u)
/* ISO-8601 UTC timestamp, e.g. "2026-08-06T13:56:00Z". */
#define ATLAS_TS_MAX 32u
/* Branch / ref shorthand. */
#define ATLAS_BRANCH_MAX 256u

#endif /* ATLAS_LIMITS_H */

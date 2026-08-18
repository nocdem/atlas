/* Atlas - umbrella header and version.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 */
#ifndef ATLAS_ATLAS_H
#define ATLAS_ATLAS_H

#define ATLAS_VERSION_MAJOR 0
#define ATLAS_VERSION_MINOR 1
#define ATLAS_VERSION_PATCH 0
#define ATLAS_VERSION_STRING "0.1.0"
#define ATLAS_PHASE "A9.2.6"

#include <stddef.h>
#include <stdint.h>

#include "atlas/buf.h"
#include "atlas/datadir.h"
#include "atlas/db.h"
#include "atlas/error.h"
#include "atlas/git.h"
#include "atlas/json.h"
#include "atlas/limits.h"
#include "atlas/pathrep.h"
#include "atlas/proc.h"
#include "atlas/scan.h"
#include "atlas/service.h"
#include "atlas/sha256.h"

/* Compiler identification captured at build time, for `atlas doctor`. */
const char *atlas_build_compiler(void);
/* UTC "YYYY-MM-DDTHH:MM:SSZ" for now, written into `out` (>= ATLAS_TS_MAX). */
void atlas_now_iso8601(char *out, size_t out_size);
/* The same format, `ms_ago` milliseconds in the past. Used for idle-expiry
 * cutoffs, which compare as strings because the format sorts lexicographically.
 * A cutoff earlier than the epoch is clamped rather than wrapped. */
void atlas_iso8601_before_now(char *out, size_t out_size, int64_t ms_ago);
/* The same format, `ms_ahead` milliseconds in the future, for the expiry of a
 * short-lived capability. Clamped to now rather than wrapped on overflow: a
 * capability that never expires is worse than one that has already. */
void atlas_iso8601_after_now(char *out, size_t out_size, int64_t ms_ahead);

#endif /* ATLAS_ATLAS_H */

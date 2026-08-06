/* Atlas - small shared utilities.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 */
#include <stdio.h>
#include <time.h>

#include "atlas/atlas.h"

#ifndef ATLAS_BUILD_COMPILER
#define ATLAS_BUILD_COMPILER "unknown"
#endif

const char *atlas_build_compiler(void) {
    return ATLAS_BUILD_COMPILER;
}

/* Formats one UTC instant. Shared so the two entry points below cannot drift
 * into producing timestamps that do not compare as strings. */
static void format_iso8601(time_t when, char *out, size_t out_size) {
    struct tm tm_utc;
    if (gmtime_r(&when, &tm_utc) == NULL) {
        (void)snprintf(out, out_size, "1970-01-01T00:00:00Z");
        return;
    }
    (void)snprintf(out, out_size, "%04d-%02d-%02dT%02d:%02d:%02dZ", tm_utc.tm_year + 1900,
                   tm_utc.tm_mon + 1, tm_utc.tm_mday, tm_utc.tm_hour, tm_utc.tm_min,
                   tm_utc.tm_sec);
}

void atlas_now_iso8601(char *out, size_t out_size) {
    if (out == NULL || out_size == 0) {
        return;
    }
    format_iso8601(time(NULL), out, out_size);
}

void atlas_iso8601_before_now(char *out, size_t out_size, int64_t ms_ago) {
    if (out == NULL || out_size == 0) {
        return;
    }
    time_t now = time(NULL);
    time_t back = (time_t)(ms_ago / 1000);
    /* A cutoff that would run past the epoch is clamped rather than wrapped: an
     * expiry sweep against a negative timestamp would match everything. */
    format_iso8601(back > now ? (time_t)0 : now - back, out, out_size);
}

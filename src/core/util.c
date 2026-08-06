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

void atlas_now_iso8601(char *out, size_t out_size) {
    if (out == NULL || out_size == 0) {
        return;
    }
    time_t now = time(NULL);
    struct tm tm_utc;
    if (gmtime_r(&now, &tm_utc) == NULL) {
        (void)snprintf(out, out_size, "1970-01-01T00:00:00Z");
        return;
    }
    (void)snprintf(out, out_size, "%04d-%02d-%02dT%02d:%02d:%02dZ", tm_utc.tm_year + 1900,
                   tm_utc.tm_mon + 1, tm_utc.tm_mday, tm_utc.tm_hour, tm_utc.tm_min,
                   tm_utc.tm_sec);
}

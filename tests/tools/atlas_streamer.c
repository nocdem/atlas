/* Atlas - A11.5a-R2: a child that emits streamed progress on a schedule.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Stands in for a model worker in the one respect the idle bound cares about:
 * what it writes, when, and whether Atlas should count it as being alive.
 *
 * A real Claude Code run takes minutes and costs money, and a test that needed
 * one could not be part of a suite anybody runs. What the driver actually
 * depends on is narrow enough to reproduce exactly — records on stdout, one per
 * line, separated by a delay — so that is what this emits.
 *
 *   atlas-streamer <mode> <count> <gap_ms>
 *
 *   events   `count` well-formed stream-json records, `gap_ms` apart
 *   prose    `count` lines of ordinary output, which must not count as activity
 *   broken   `count` lines of malformed JSON, which must not either
 *   silent   nothing at all for `count * gap_ms`, then exits
 *   long     one record longer than any bound, then exits
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void nap(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    (void)nanosleep(&ts, NULL);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        (void)fprintf(stderr, "usage: atlas-streamer <mode> <count> <gap_ms>\n");
        return 2;
    }
    const char *mode = argv[1];
    long count = strtol(argv[2], NULL, 10);
    long gap = strtol(argv[3], NULL, 10);

    if (strcmp(mode, "silent") == 0) {
        nap(gap * (count > 0 ? count : 1));
        return 0;
    }
    if (strcmp(mode, "long") == 0) {
        (void)fputs("{\"type\":\"assistant\",\"pad\":\"", stdout);
        for (long i = 0; i < 200000; i++) {
            (void)fputc('x', stdout);
        }
        (void)fputs("\"}\n", stdout);
        (void)fflush(stdout);
        return 0;
    }
    for (long i = 0; i < count; i++) {
        nap(gap);
        if (strcmp(mode, "events") == 0) {
            /* Shaped like the records the installed CLI emits, alternating a
             * plain assistant turn with one carrying a tool use. */
            if (i % 2 == 0) {
                (void)printf("{\"type\":\"assistant\",\"message\":{\"content\":[{\"type\":"
                             "\"text\",\"text\":\"step %ld\"}]}}\n",
                             i);
            } else {
                (void)printf("{\"type\":\"assistant\",\"message\":{\"content\":[{\"type\":"
                             "\"tool_use\",\"name\":\"Read\",\"id\":\"t%ld\"}]}}\n",
                             i);
            }
        } else if (strcmp(mode, "prose") == 0) {
            (void)printf("still working, please wait (%ld)\n", i);
        } else { /* broken */
            (void)printf("{\"type\":\"assistant\",\"message\":{\"content\":[\n");
        }
        (void)fflush(stdout);
    }
    return 0;
}

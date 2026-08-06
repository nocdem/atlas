/* Atlas - test harness implementation.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 */
#include "atlas_test.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

#include "support/fixture.h"

static jmp_buf g_abort;
static int g_current_failures;
static bool g_in_test;
/* File scope so longjmp() cannot clobber the running tally. */
static size_t g_failed_tests;

void atlas_test_fail(const char *file, int line, const char *fmt, ...) {
    g_current_failures++;
    (void)fprintf(stderr, "    FAIL %s:%d: ", file, line);
    va_list ap;
    va_start(ap, fmt);
    (void)vfprintf(stderr, fmt, ap);
    va_end(ap);
    (void)fputc('\n', stderr);
    (void)fflush(stderr);
}

void atlas_test_note(const char *fmt, ...) {
    (void)fprintf(stderr, "    note: ");
    va_list ap;
    va_start(ap, fmt);
    (void)vfprintf(stderr, fmt, ap);
    va_end(ap);
    (void)fputc('\n', stderr);
    (void)fflush(stderr);
}

void atlas_test_abort(void) {
    if (g_in_test) {
        longjmp(g_abort, 1);
    }
    exit(1);
}

int atlas_test_run(const char *suite, const atlas_test *tests, size_t count) {
    g_failed_tests = 0;
    (void)fprintf(stderr, "== %s: %zu tests\n", suite, count);
    for (size_t i = 0; i < count; i++) {
        g_current_failures = 0;
        g_in_test = true;
        if (setjmp(g_abort) == 0) {
            tests[i].fn();
        }
        g_in_test = false;
        /* A test that abandoned its body never reached its own cleanup. */
        fx_cleanup_leaked();
        if (g_current_failures == 0) {
            (void)fprintf(stderr, "  ok   %s\n", tests[i].name);
        } else {
            (void)fprintf(stderr, "  FAIL %s (%d problem%s)\n", tests[i].name, g_current_failures,
                          g_current_failures == 1 ? "" : "s");
            g_failed_tests++;
        }
    }
    if (g_failed_tests == 0) {
        (void)fprintf(stderr, "== %s: all %zu tests passed\n", suite, count);
    } else {
        (void)fprintf(stderr, "== %s: %zu of %zu tests FAILED\n", suite, g_failed_tests, count);
    }
    return g_failed_tests == 0 ? 0 : 1;
}

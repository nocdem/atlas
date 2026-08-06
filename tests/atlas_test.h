/* Atlas - first-party test harness.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Deliberately dependency-free: the A0 build must not require a test framework
 * that is not already guaranteed by the environment.
 *
 * T_CHECK records a failure and keeps going, so one test can report several
 * problems. T_REQUIRE records a failure and abandons the current test, for cases
 * where continuing would crash or report noise.
 */
#ifndef ATLAS_TEST_H
#define ATLAS_TEST_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef void (*atlas_test_fn)(void);

typedef struct atlas_test {
    const char *name;
    atlas_test_fn fn;
} atlas_test;

/* Runs every test and returns the number of failed tests. */
int atlas_test_run(const char *suite, const atlas_test *tests, size_t count);

void atlas_test_fail(const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void atlas_test_abort(void) __attribute__((noreturn));
/* Prints an informational line under the current test. */
void atlas_test_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define T_CHECK(cond)                                                        \
    do {                                                                     \
        if (!(cond)) {                                                        \
            atlas_test_fail(__FILE__, __LINE__, "check failed: %s", #cond);   \
        }                                                                     \
    } while (0)

#define T_CHECK_MSG(cond, ...)                                  \
    do {                                                        \
        if (!(cond)) {                                          \
            atlas_test_fail(__FILE__, __LINE__, __VA_ARGS__);    \
        }                                                        \
    } while (0)

#define T_REQUIRE(cond)                                                        \
    do {                                                                       \
        if (!(cond)) {                                                         \
            atlas_test_fail(__FILE__, __LINE__, "require failed: %s", #cond);   \
            atlas_test_abort();                                                 \
        }                                                                       \
    } while (0)

#define T_REQUIRE_MSG(cond, ...)                                \
    do {                                                        \
        if (!(cond)) {                                          \
            atlas_test_fail(__FILE__, __LINE__, __VA_ARGS__);    \
            atlas_test_abort();                                 \
        }                                                        \
    } while (0)

#define T_EQ_INT(actual, expected)                                                     \
    do {                                                                               \
        long long a_ = (long long)(actual);                                            \
        long long e_ = (long long)(expected);                                          \
        if (a_ != e_) {                                                                \
            atlas_test_fail(__FILE__, __LINE__, "%s: expected %lld, got %lld",          \
                            #actual, e_, a_);                                          \
        }                                                                              \
    } while (0)

#define T_EQ_STR(actual, expected)                                                       \
    do {                                                                                 \
        const char *a_ = (actual);                                                       \
        const char *e_ = (expected);                                                     \
        if (a_ == NULL || e_ == NULL || strcmp(a_, e_) != 0) {                            \
            atlas_test_fail(__FILE__, __LINE__, "%s: expected \"%s\", got \"%s\"",        \
                            #actual, e_ != NULL ? e_ : "(null)", a_ != NULL ? a_ : "(null)"); \
        }                                                                                 \
    } while (0)

#define T_EQ_MEM(actual, actual_len, expected, expected_len)                              \
    do {                                                                                  \
        size_t al_ = (size_t)(actual_len);                                                \
        size_t el_ = (size_t)(expected_len);                                              \
        if (al_ != el_ || memcmp((actual), (expected), el_) != 0) {                        \
            atlas_test_fail(__FILE__, __LINE__, "%s: byte content differs (%zu vs %zu bytes)", \
                            #actual, al_, el_);                                           \
        }                                                                                 \
    } while (0)

/* Asserts that a call returned ATLAS_OK, reporting the error message if not. */
#define T_OK(expr, errp)                                                                 \
    do {                                                                                 \
        atlas_status st_ = (expr);                                                       \
        if (st_ != ATLAS_OK) {                                                           \
            atlas_test_fail(__FILE__, __LINE__, "%s failed: %s (%s)", #expr,              \
                            atlas_err_msg(errp), atlas_status_name(st_));                 \
            atlas_test_abort();                                                          \
        }                                                                                 \
    } while (0)

/* Asserts that a call failed with the given status. */
#define T_FAILS_WITH(expr, expected_status, errp)                                        \
    do {                                                                                 \
        atlas_status st_ = (expr);                                                       \
        if (st_ != (expected_status)) {                                                   \
            atlas_test_fail(__FILE__, __LINE__, "%s: expected %s, got %s (%s)", #expr,     \
                            atlas_status_name(expected_status), atlas_status_name(st_),     \
                            atlas_err_msg(errp));                                         \
        }                                                                                 \
    } while (0)

#define ATLAS_TEST_MAIN(suite, tests)                                              \
    int main(void) {                                                               \
        return atlas_test_run(suite, tests, sizeof(tests) / sizeof(tests[0]));      \
    }

#endif /* ATLAS_TEST_H */

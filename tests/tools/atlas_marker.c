/* Atlas - marker helper for adversarial hardening tests.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Stands in for a hostile helper that a repository's own configuration asks git to
 * execute: an fsmonitor hook, an external diff driver, a textconv filter, a pager,
 * an askpass program, or a repository hook. When run it appends a line to
 * "<own path>.fired" and exits 0.
 *
 * Deriving the marker path from argv[0] means the configuration value is a single
 * absolute path with no arguments and no shell metacharacters, so git execs it
 * directly. If this program ever runs, the test has found a real hole.
 */
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    char path[4096];
    const char *self = (argc > 0 && argv[0] != NULL) ? argv[0] : "atlas-marker";
    if (snprintf(path, sizeof(path), "%s.fired", self) >= (int)sizeof(path)) {
        return 0; /* nothing useful to do; never fail the caller */
    }
    FILE *f = fopen(path, "ae");
    if (f == NULL) {
        return 0;
    }
    /* Record how it was invoked, so a failing test can say which vector fired. */
    (void)fprintf(f, "fired argc=%d", argc);
    for (int i = 1; i < argc && i < 8; i++) {
        (void)fprintf(f, " arg%d=%s", i, argv[i] != NULL ? argv[i] : "");
    }
    (void)fputc('\n', f);
    (void)fclose(f);
    return 0;
}

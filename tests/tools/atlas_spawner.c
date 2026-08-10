/* Atlas - a test helper that forks a grandchild and outlives nothing.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Exists for exactly one assertion: that `atlas_proc_run` terminates the whole
 * process *group* rather than just the child it started. It forks a grandchild
 * which writes its pid to a file and then sleeps for far longer than any test
 * would wait, and the parent sleeps too. If Atlas killed only the direct child,
 * the grandchild would survive the run and the test would find it alive.
 *
 * argv[1] is a file the grandchild writes its pid into.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        return 2;
    }
    pid_t kid = fork();
    if (kid == 0) {
        FILE *f = fopen(argv[1], "w");
        if (f != NULL) {
            (void)fprintf(f, "%lld\n", (long long)getpid());
            (void)fclose(f);
        }
        /* Longer than any test waits, so surviving is unambiguous. */
        (void)sleep(3600);
        _exit(0);
    }
    /* Say something so an idle-timeout test can distinguish "silent" from
     * "not started", then block. */
    (void)printf("spawned\n");
    (void)fflush(stdout);
    (void)sleep(3600);
    return 0;
}

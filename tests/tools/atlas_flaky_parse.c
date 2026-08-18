/* Atlas - a parse child that fails transiently before it succeeds.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A9.2.5's per-unit retry is load-bearing scheduler behaviour: a translation
 * unit whose parse child was OOM-killed used to cost a repository the ability to
 * state an absence until somebody happened to edit a file. Behaviour like that
 * has to be tested end to end, and the closure report is not allowed to record
 * "untestable" as a limit.
 *
 * **This adds nothing to production Atlas.** `atlas_sem_index_opts.atlas_exe`
 * has been the parse child's executable path since A8-CI, because the child is
 * Atlas re-executed; a test that supplies a different path is using the
 * parameter, not bypassing a check. Nothing here relaxes an authority boundary:
 * the indexer still spawns exactly one process per unit through
 * `atlas_proc_run`, with an explicit argv array and an absolute argv[0], into an
 * empty environment.
 *
 * The contract it emulates is the one `atlas_sem_parse_unit` actually tests for:
 *
 *     if (pr.exit_code != 0 || pr.term_signal != 0 || out.len == 0) {
 *         res->status = ATLAS_SEM_TU_FAILED;
 *         res->why    = ATLAS_SEM_WHY_CHILD_FAILED;
 *
 * So "fail transiently" is exactly "exit non-zero, write nothing", which is what
 * an OOM-killed or crashed child looks like from the parent's side.
 *
 * State lives in files beside this executable rather than in the environment,
 * because the indexer gives the child an **empty** environment on purpose and a
 * helper that needed a variable would be evidence that the seam was wrong. The
 * caller copies this binary into its own fixture directory, so two tests running
 * at once cannot share a counter.
 *
 *   <argv0>.failures  how many leading invocations must fail (default 1)
 *   <argv0>.calls     how many invocations have happened, for the test to assert
 *                     that the bound was respected
 *
 * After the failing prefix it becomes the real thing by `execv`-ing the Atlas
 * binary with the argument vector it was handed, so the successful attempt is a
 * genuine libclang parse and the facts it emits are real.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static long read_long(const char *path, long fallback) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return fallback;
    }
    long v = fallback;
    if (fscanf(f, "%ld", &v) != 1) {
        v = fallback;
    }
    (void)fclose(f);
    return v;
}

int main(int argc, char **argv) {
    if (argc < 1 || argv[0] == NULL) {
        return 2;
    }

    char calls_path[4096];
    char fail_path[4096];
    if ((size_t)snprintf(calls_path, sizeof calls_path, "%s.calls", argv[0]) >=
            sizeof calls_path ||
        (size_t)snprintf(fail_path, sizeof fail_path, "%s.failures", argv[0]) >=
            sizeof fail_path) {
        return 2;
    }

    const long calls = read_long(calls_path, 0) + 1;
    FILE *f = fopen(calls_path, "w");
    if (f != NULL) {
        (void)fprintf(f, "%ld\n", calls);
        (void)fclose(f);
    }

    /* Default one failure: enough to exercise the retry, and the bound the
     * indexer allows, so a test that forgets to write the file still gets the
     * interesting case rather than an infinite one. */
    if (calls <= read_long(fail_path, 1)) {
        /* Non-zero exit, nothing on stdout. `ATLAS_SEM_WHY_CHILD_FAILED`. */
        return 3;
    }

    /* Become the real parse child. argv[0] is this helper's path and Atlas reads
     * only argv[1] onwards, so the vector passes through unchanged. */
    (void)execv(ATLAS_BIN, argv);
    return 127;
}

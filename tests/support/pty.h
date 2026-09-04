/* Atlas - a real pseudo-terminal for the operator-channel test suites.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Moved out of tests/test_decision_operator.c (A15 T6) so a second suite --
 * tests/test_review_apply.c -- can drive the same terminal-allocation trick
 * without a second, drifting copy. Unchanged in behaviour at the move, apart
 * from the one signature difference the move itself needed -- `pty_spawn` no
 * longer takes this suite's own `env` fixture type, which test_review_apply.c
 * does not share; it takes the two things `env` was only ever used for here,
 * the data directory and the binary to execve.
 *
 * `pty_wait` gained a bound after the move (T6 fix round 1): a suite driving
 * a real, unlocked walker can reach a state where the walker is wrongly
 * still prompting when the sheet is exhausted, which blocks the child on
 * `/dev/tty` forever. See `pty_wait`'s own comment.
 *
 * See test_decision_operator.c's own header comment for what this
 * demonstrates and why: a program allocating a pseudo-terminal and typing at
 * it is indistinguishable, from inside Atlas, from a person at a keyboard.
 * `posix_openpt` / `grantpt` / `unlockpt` / `ptsname_r` are POSIX and in libc.
 * No `-lutil`, no `forkpty`, no new dependency.
 */
#ifndef ATLAS_TEST_SUPPORT_PTY_H
#define ATLAS_TEST_SUPPORT_PTY_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "atlas/buf.h"
#include "atlas/error.h"

typedef struct pty {
    int master;
    pid_t child;
} pty;

/* `pty_wait`'s sentinel for "the child did not exit within the bound and was
 * killed" -- outside the 0-255 exit-code range and outside 128+signal, so a
 * caller can tell a timeout apart from any real exit unambiguously. */
#define PTY_WAIT_TIMED_OUT (-1000)

/* Forks `bin_path` with a pseudo-terminal as its stdin, stdout and stderr, and
 * `--data-dir data_dir` prepended to `args`. The child calls `setsid()` and
 * then `ioctl(TIOCSCTTY)`, which is what makes the slave its *controlling*
 * terminal -- without that, `/dev/tty` in the child would still refer to the
 * terminal the test runner was started from, or to nothing at all under
 * CTest. That distinction is the whole point: Atlas reads the confirmation
 * from `/dev/tty`, not from standard input.
 *
 * `HOME` is set to `data_dir` and the rest of the environment is an
 * explicitly constructed, minimal list -- nothing inherited, so no ambient
 * variable can influence the child, matching the rest of the suite. */
atlas_status pty_spawn(const char *data_dir, const char *bin_path, const char *const *args,
                       size_t nargs, pty *out, atlas_err *err);

/* Reads from the terminal until `needle` appears or the child exits. Bounded
 * by an absolute deadline rather than by a read count: a hung child must fail
 * the test rather than block CTest until its own timeout. Accumulates into
 * `transcript`, so a needle already read on a prior call is found again
 * without a second read -- callers that must tell two prompts apart need to
 * count occurrences in the transcript themselves rather than call this twice
 * for the same needle. */
bool pty_expect(pty *p, const char *needle, atlas_buf *transcript);

/* Writes `line` plus a trailing newline to the terminal, as if typed. */
void pty_type(pty *p, const char *line);

/* Drains whatever output is left, then reaps the child. Returns its exit
 * status (128 + signal number if it died from one), or `PTY_WAIT_TIMED_OUT`
 * if the child was still running after a bounded wait -- a walker that
 * wrongly prompts leaves the child blocked reading `/dev/tty` forever, and
 * this must fail with a message rather than hang the whole test process to
 * CTest's own timeout. On a timeout the child has already been SIGKILLed and
 * reaped. */
int pty_wait(pty *p, atlas_buf *transcript);

#endif /* ATLAS_TEST_SUPPORT_PTY_H */

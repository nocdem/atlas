/* Atlas - the operator-only interactive channel.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The one place in Atlas that requires a human-shaped input, and the one place
 * whose limitations have to be stated as plainly as its behaviour.
 *
 * **What this establishes.** That the confirmation was typed at a controlling
 * terminal, not supplied on standard input, not read from a file, not taken
 * from an environment variable, not parsed out of a JSON request, and not
 * implied by `--yes`. The prompt is written to the terminal device and the
 * answer is read back from the same device, so a pipeline feeding the process
 * cannot answer it.
 *
 * **What this does not establish, and never will.** Which person was at the
 * keyboard, or that a person was there at all. Any process running as the same
 * user can allocate a pseudo-terminal, run this binary against it and type the
 * confirmation — Atlas' own PTY tests do exactly that, which is the honest
 * demonstration that the boundary is a channel rather than an identity. There
 * is no cryptography here, no signature, no non-repudiation, and no attempt to
 * imply any. A4 adds no signing keys and no hardware-token support; adding the
 * vocabulary without the mechanism would be worse than the current claim.
 *
 * What the channel is worth is what it *excludes*: an approval cannot be
 * produced by a model's text, by a hook payload, by an MCP tool call, by a
 * repository file, by an environment variable, or by replaying a captured
 * request. Those are checkable properties, and they are the whole claim.
 */
#ifndef ATLAS_TERMINAL_H
#define ATLAS_TERMINAL_H

#include <stdbool.h>
#include <stddef.h>

#include "atlas/buf.h"
#include "atlas/error.h"

typedef struct atlas_terminal atlas_terminal;

/* True when this process has a controlling terminal on standard input *and*
 * standard output.
 *
 * Both, deliberately. Requiring only the read side would let `atlas decision
 * approve ... > file` hide the prompt while still accepting the answer, and a
 * confirmation nobody saw is not a confirmation. This is a cheap pre-check so
 * that a non-interactive caller gets a clear refusal before a challenge is
 * issued; the real gate is `atlas_terminal_open`. */
bool atlas_terminal_available(void);

/* Opens `/dev/tty` for reading and writing.
 *
 * Not standard input. That is the point: standard input may be a pipe, a file
 * or `/dev/null`, and any of those could carry an answer that no person typed.
 * `/dev/tty` is the process's controlling terminal or nothing, so a process
 * without one cannot open it at all.
 *
 * Fails with ATLAS_ERR_USAGE — not an internal error — when there is no
 * controlling terminal, because being run non-interactively is a usage mistake
 * with an obvious remedy rather than a fault in Atlas. */
atlas_status atlas_terminal_open(atlas_terminal **out, atlas_err *err);
void atlas_terminal_close(atlas_terminal *t);

/* Writes to the terminal device.
 *
 * Every caller must have encoded untrusted text with `atlas_safe()` first.
 * This function does not encode, and deliberately does not: an encoder here
 * would be a second, quieter one, and the approval display is exactly the place
 * where "somebody else encoded it" must not be an assumption. What it does do
 * is refuse to emit a byte a terminal would act on — see
 * `atlas_terminal_byte_ok` — so a caller that forgot gets a visible marker
 * rather than a working escape sequence. */
atlas_status atlas_terminal_write(atlas_terminal *t, const char *text, size_t len,
                                  atlas_err *err);
atlas_status atlas_terminal_writef(atlas_terminal *t, atlas_err *err, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* The output allowlist, exposed so tests assert the policy rather than a list
 * of examples. Printable ASCII, space and newline; nothing else. */
bool atlas_terminal_byte_ok(unsigned char c);

/* Reads one line from the terminal device, without echoing anything itself.
 *
 * Bounded by `max` bytes; a longer line is an error rather than a truncation,
 * because a truncated confirmation that happened to match a prefix would be a
 * confirmation the operator did not give. The trailing newline is removed and
 * surrounding whitespace is trimmed; nothing else about the input is
 * interpreted, and it never reaches a parser. */
atlas_status atlas_terminal_read_line(atlas_terminal *t, atlas_buf *out, size_t max,
                                      atlas_err *err);

#endif /* ATLAS_TERMINAL_H */

/* Atlas - the Claude Code hook adapter.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * `atlas hook <event>` reads one hook payload on stdin and writes exactly one
 * JSON object on stdout. It is the deterministic half of the A2 integration:
 * hooks make Atlas *participate* without being asked, and MCP is what Claude
 * reaches for when it wants to know or record something specific.
 *
 * Three properties govern every event, and they are properties of the process
 * rather than of any individual handler:
 *
 * 1. **It fails open.** A missing daemon, a timeout, a malformed payload and an
 *    unknown event all produce a valid, minimal document and exit 0. Atlas is
 *    engineering memory; it is not something a person should have to disable to
 *    get their work done. The failure that matters is a *silent* one, so a hook
 *    that could not record something says so on stderr and records nothing,
 *    rather than recording a guess.
 *
 * 2. **It is fast, or it gives up.** Every daemon call carries a deadline far
 *    below Claude's timeout (ATLAS_HOOK_IPC_TIMEOUT_MS, and a stricter one at
 *    session end where all hooks share a 1.5 second budget). No hook performs a
 *    repository scan; the most any of them does is ask the daemon to schedule
 *    one.
 *
 * 3. **It stores metadata, never content.** No prompt, no assistant message, no
 *    transcript, no tool input, no tool result, no error text, no shell command,
 *    no environment variable. What a hook extracts from a payload is: which
 *    session, which tool, whether it succeeded, and at most one normalized path.
 *    docs/ai-trust-boundary.md lists this exhaustively and the test suite
 *    asserts it against payloads that contain all of it.
 */
#ifndef ATLAS_HOOK_H
#define ATLAS_HOOK_H

#include <stdbool.h>
#include <stdio.h>

#include "atlas/error.h"

typedef struct atlas_hook_opts {
    /* Overrides the socket path. Tests set this; nothing else does. */
    const char *socket_path;
    /* Milliseconds a daemon call may take before the hook gives up and fails
     * open. Zero means the event's own default. */
    int timeout_ms;
} atlas_hook_opts;

void atlas_hook_opts_init(atlas_hook_opts *o);

/* Handles one event. Always writes one JSON object to `out` and always returns
 * ATLAS_OK: a hook that reports failure to Claude has made a person's session
 * worse for a reason that is Atlas' problem, not theirs. Diagnostics go to
 * `errout`, which Claude shows only in debug mode. */
atlas_status atlas_hook_run(const char *event, FILE *in, FILE *out, FILE *errout,
                            const atlas_hook_opts *opts);

/* True when `event` is one Atlas handles. An unknown event is still answered
 * with `{}`; this exists so `atlas integrate claude doctor` can report which
 * events the installed plugin configures against what this binary knows. */
bool atlas_hook_event_known(const char *event);

/* The events Atlas handles, for `doctor` and for the plugin's own tests.
 * NULL-terminated, in the order they appear in the plugin's hooks.json. */
const char *const *atlas_hook_events(void);

#endif /* ATLAS_HOOK_H */

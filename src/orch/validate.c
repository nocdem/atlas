/* Atlas - A8/A11.1: running a job's declared verification commands.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See atlas/validate.h. This is the whole of it; there is no second implementation
 * and adding one is what the header forbids.
 */
#define _GNU_SOURCE 1

#include "atlas/validate.h"

#include <string.h>

#include "atlas/workspace.h"

void atlas_validation_result_init(atlas_validation_result *r) {
    memset(r, 0, sizeof(*r));
    r->failed_index = -1;
    atlas_buf_init(&r->output);
}

void atlas_validation_result_free(atlas_validation_result *r) {
    if (r != NULL) {
        atlas_buf_free(&r->output);
    }
}

bool atlas_validation_program_allowed(const char *name) {
    static const char *const ALLOWED[] = {"make", "ctest", "cmake", "true", "false"};
    if (name == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof ALLOWED / sizeof ALLOWED[0]; i++) {
        if (strcmp(name, ALLOWED[i]) == 0) {
            return true;
        }
    }
    return false;
}

atlas_status atlas_validations_run(const atlas_orch_argv *cmds, size_t n, const atlas_validation_opts *o,
                             atlas_validation_result *out, atlas_err *err) {
    out->passed = true;
    out->failed_index = -1;
    out->ran = 0;

    for (size_t i = 0; i < n && out->passed; i++) {
        if (cmds[i].count == 0) {
            out->passed = false;
            out->failed_index = (int64_t)i;
            return atlas_buf_set_str(&out->output, "the command is empty\n", err);
        }
        const char *prog = atlas_buf_cstr(&cmds[i].args[0]);
        if (!atlas_validation_program_allowed(prog)) {
            out->passed = false;
            out->failed_index = (int64_t)i;
            return atlas_buf_set_str(&out->output,
                                     "refused: the program is not on the allowlist\n", err);
        }

        atlas_buf exe = ATLAS_BUF_INIT;
        atlas_status st = atlas_proc_which(prog, "/usr/local/bin:/usr/bin:/bin", &exe, err);
        if (st != ATLAS_OK) {
            out->passed = false;
            out->failed_index = (int64_t)i;
            atlas_err_init(err);
            atlas_buf_free(&exe);
            return atlas_buf_set_str(&out->output, "the program could not be resolved\n", err);
        }

        const char *argv[ATLAS_ORCH_MAX_ARGV + 2u];
        size_t k = 0;
        argv[k++] = atlas_buf_cstr(&exe);
        for (size_t v = 1; v < cmds[i].count; v++) {
            argv[k++] = atlas_buf_cstr(&cmds[i].args[v]);
        }
        argv[k] = NULL;

        /* A clean, explicitly built environment. Nothing inherited: no SSH
         * agent, no sudo askpass, no credential, no operator configuration. */
        static const char *const ENV[] = {"PATH=/usr/local/bin:/usr/bin:/bin", "LC_ALL=C",
                                          "LANG=C", "TZ=UTC", NULL};
        atlas_buf raw = ATLAS_BUF_INIT;
        atlas_buf errout = ATLAS_BUF_INIT;
        atlas_proc_opts opts;
        memset(&opts, 0, sizeof(opts));
        opts.argv = argv;
        opts.env = ENV;
        opts.cwd = o->cwd;
        opts.timeout_ms = (int)o->wall_timeout_ms;
        opts.idle_timeout_ms = (int)o->idle_timeout_ms;
        opts.max_stdout = (size_t)o->max_output_bytes;
        opts.max_stderr = 256u * 1024u;
        opts.cancel = o->cancel;
        opts.cancel_ud = o->cancel_ud;
        atlas_proc_result pr;
        memset(&pr, 0, sizeof(pr));
        atlas_status rs = atlas_proc_run(&opts, atlas_proc_sink_buf, &raw, &errout, &pr, err);
        out->ran = i + 1u;

        /* Redacted before it reaches any destination. A validation command's
         * output is as likely to echo an environment as a driver's is. */
        atlas_buf clean = ATLAS_BUF_INIT;
        atlas_err ignore;
        atlas_err_init(&ignore);
        if (atlas_ws_redact(raw.data != NULL ? raw.data : "", raw.len, &clean, NULL, &ignore) !=
            ATLAS_OK) {
            atlas_buf_reset(&clean);
        }
        /* stderr matters as much as stdout for a gate: a compiler says why it
         * refused on the second one. Appended after, and redacted the same. */
        if (errout.len > 0) {
            atlas_buf cleanerr = ATLAS_BUF_INIT;
            if (atlas_ws_redact(errout.data, errout.len, &cleanerr, NULL, &ignore) == ATLAS_OK) {
                (void)atlas_buf_append(&clean, cleanerr.data, cleanerr.len, &ignore);
            }
            atlas_buf_free(&cleanerr);
        }
        if (o->log != NULL) {
            atlas_status ls = o->log(i, &clean, o->log_ud, err);
            if (ls != ATLAS_OK) {
                atlas_buf_free(&clean);
                atlas_buf_free(&raw);
                atlas_buf_free(&errout);
                atlas_buf_free(&exe);
                return ls;
            }
        }

        bool failed = rs != ATLAS_OK || pr.exit_code != 0 || pr.timed_out || pr.idle_timed_out;
        if (failed) {
            out->passed = false;
            out->failed_index = (int64_t)i;
            /* A failing gate is an answer, not an error of Atlas'. The status is
             * cleared and the outcome travels in the result, which is the same
             * arrangement `atlas_driver` uses for a bounded or cancelled run. */
            atlas_err_init(err);
            atlas_status cs = atlas_buf_set(&out->output, clean.data, clean.len, err);
            atlas_buf_free(&clean);
            atlas_buf_free(&raw);
            atlas_buf_free(&errout);
            atlas_buf_free(&exe);
            return cs;
        }
        atlas_buf_free(&clean);
        atlas_buf_free(&raw);
        atlas_buf_free(&errout);
        atlas_buf_free(&exe);
    }
    return ATLAS_OK;
}

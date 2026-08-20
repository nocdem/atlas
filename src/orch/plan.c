/* Atlas - A12.0: the `atlas-plan-1` parser and the planned run's five prompts.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See atlas/plan.h for what a plan is and for the three things it is not. This
 * file is **pure**: no database handle, no process, no file descriptor, no
 * clock — the `src/orch/memory.c` discipline, and for the same reason. A
 * refusal a driver quotes back to a model as a retry prompt has to be the same
 * refusal on the next machine and the next day, or the retry is not a retry.
 *
 * Two things deliberately do not live here:
 *
 * - **The gate allowlist.** `atlas_validation_program_allowed` in
 *   `src/orch/validate.c` is the one implementation, and this file calls it. A
 *   second copy is a second place for one of them to grow a program.
 * - **The gate split.** `atlas_orch_gate_split` is the splitter an operator's
 *   `--gate` goes through, lifted out of `src/core/service_orch.c` by this
 *   season so a planner's `gate:` line reaches the allowlist as the same argv
 *   an operator's flag would. Two splitters would be two answers to "what is
 *   argv[0] here", and argv[0] is what the allowlist is applied to.
 *
 * The format specification the planner is shown is a string constant in this
 * file, a few dozen lines below the parser that enforces it. It states numbers
 * rather than macro names because a model reads a number; the `_Static_assert`s
 * beside it fail the build if a bound moves without the sentence moving. A spec
 * that has drifted from its parser is worse than no spec: it makes a correct
 * planner produce a refused document.
 */
#define _GNU_SOURCE 1

#include "atlas/plan.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "atlas/validate.h"

/* --- the bounds, pinned to the sentences the planner is shown -------------
 *
 * PLAN_FORMAT_SPEC below writes these out as digits. If one of them moves,
 * this build stops until the sentence moves with it. */
_Static_assert(ATLAS_PLAN_MAX_LINE == 4096, "PLAN_FORMAT_SPEC states 4096 bytes per line");
_Static_assert(ATLAS_PLAN_TITLE_MAX == 200, "PLAN_FORMAT_SPEC states 200 bytes per title");
_Static_assert(ATLAS_PLAN_TASK_PROMPT_MAX == 16384, "PLAN_FORMAT_SPEC states 16384 per prompt");
_Static_assert(ATLAS_PLAN_MAX_STAGES == 4, "PLAN_FORMAT_SPEC states at most 4 stages");
_Static_assert(ATLAS_PLAN_MAX_SIDE_PER_STAGE == 3, "PLAN_FORMAT_SPEC states 0..3 side tasks");
_Static_assert(ATLAS_PLAN_MAX_TASKS == 8, "PLAN_FORMAT_SPEC states at most 8 tasks");
_Static_assert(ATLAS_ORCH_MAX_VALIDATIONS == 8u, "PLAN_FORMAT_SPEC states at most 8 gates");
_Static_assert(ATLAS_PLAN_MAX_BYTES == 65536, "PLAN_FORMAT_SPEC states at most 65536 bytes");
_Static_assert(sizeof(((atlas_plan_doc_task *)0)->key) == 33u,
               "PLAN_FORMAT_SPEC states [a-z0-9-]{1,32}");

const char *atlas_plan_status_name(atlas_plan_status s) {
    /* No `default:`. Adding a member is a build failure here rather than a
     * silent "UNKNOWN" on a surface somebody trusts. */
    switch (s) {
    case ATLAS_PLAN_STATUS_PLANNING: return "PLANNING";
    case ATLAS_PLAN_STATUS_EXECUTING: return "EXECUTING";
    case ATLAS_PLAN_STATUS_NEEDS_REPLAN: return "NEEDS_REPLAN";
    case ATLAS_PLAN_STATUS_COMPLETED: return "COMPLETED";
    case ATLAS_PLAN_STATUS_BLOCKED: return "BLOCKED";
    case ATLAS_PLAN_STATUS_UNKNOWN: break;
    }
    return "UNKNOWN";
}

void atlas_plan_doc_free(atlas_plan_doc *d) {
    if (d == NULL) {
        return;
    }
    for (size_t i = 0; i < ATLAS_PLAN_MAX_TASKS; i++) {
        atlas_buf_free(&d->tasks[i].prompt);
        for (size_t g = 0; g < ATLAS_ORCH_MAX_VALIDATIONS; g++) {
            atlas_orch_argv_free(&d->tasks[i].gates[g]);
        }
    }
    /* Zeroed rather than left half-released, which is what makes a second call
     * safe: a zeroed `atlas_buf` is exactly `ATLAS_BUF_INIT`, and a zeroed
     * `atlas_orch_argv` is exactly what `atlas_orch_argv_init` produces. */
    memset(d, 0, sizeof(*d));
}

/* --- the parser ----------------------------------------------------------- */

/* Records the 1-based line a refusal is about — 0 for the document as a whole —
 * and the sentence saying what. Every refusal path in this file goes through
 * here, so `*line_out` cannot be left unset by one of them. */
__attribute__((format(printf, 4, 5))) static atlas_status refuse(int *line_out, int line,
                                                                 atlas_err *err, const char *fmt,
                                                                 ...) {
    if (line_out != NULL) {
        *line_out = line;
    }
    va_list ap;
    va_start(ap, fmt);
    atlas_status st = atlas_err_setv(err, ATLAS_ERR_USAGE, fmt, ap);
    va_end(ap);
    return st;
}

/* Side tasks a stage may hold: the plan's own ceiling, and never more than the
 * run's parallelism leaves room for. Asked in one place so the number the
 * planner is told and the number the parser enforces cannot differ. */
static int side_bound(int max_parallel) {
    int by_parallel = max_parallel - 1;
    return by_parallel < ATLAS_PLAN_MAX_SIDE_PER_STAGE ? by_parallel
                                                       : ATLAS_PLAN_MAX_SIDE_PER_STAGE;
}

typedef struct plan_parse {
    atlas_plan_doc doc;
    /* min(ATLAS_PLAN_MAX_SIDE_PER_STAGE, max_parallel - 1). */
    int max_side;
    int stage_no;        /* the open stage, 0 = none */
    int stage_open_line; /* the `stage:` line that opened it */
    int tree_in_stage;
    int side_in_stage;
    int cur;      /* index of the open task in doc.tasks, -1 = none */
    int cur_line; /* the `task:` line that opened it */
    bool have_kind;
    bool have_title;
    bool have_prompt;
    bool in_heredoc;
    int heredoc_line;
} plan_parse;

/* True when `line` begins with `prefix`; `*rest` receives what follows. The
 * prefix always ends in a space, so `stage:1` is not a `stage:` line — it is an
 * unrecognised one, which is a refusal. A format with two spellings of a field
 * is a format two parsers can disagree about. */
static bool eat(const char *line, const char *prefix, const char **rest) {
    size_t n = strlen(prefix);
    if (strncmp(line, prefix, n) != 0) {
        return false;
    }
    *rest = line + n;
    return true;
}

/* `[a-z0-9-]{1,32}`. Narrow on purpose: a key is concatenated into a
 * correlation string and an idempotency key, and a key that could contain a
 * colon could impersonate another plan's job. */
static bool key_is_well_formed(const char *k, size_t n) {
    if (n == 0 || n > 32u) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = k[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

/* Every task field is required, so a task is closed by checking that all four
 * arrived. The refusal names the `task:` line rather than the line that closed
 * the task, because that is where the missing field belongs. */
static atlas_status close_task(plan_parse *c, int *line_out, atlas_err *err) {
    if (c->cur < 0) {
        return ATLAS_OK;
    }
    const char *key = c->doc.tasks[c->cur].key;
    if (!c->have_kind) {
        return refuse(line_out, c->cur_line, err, "task %s declares no kind: line", key);
    }
    if (!c->have_title) {
        return refuse(line_out, c->cur_line, err, "task %s declares no title: line", key);
    }
    if (!c->have_prompt) {
        return refuse(line_out, c->cur_line, err, "task %s declares no prompt<< block", key);
    }
    c->cur = -1;
    return ATLAS_OK;
}

static atlas_status parse_stage(plan_parse *c, const char *rest, int lineno, int *line_out,
                                atlas_err *err) {
    if (rest[0] == '\0') {
        return refuse(line_out, lineno, err, "a stage: line needs a number");
    }
    int v = 0;
    for (const char *p = rest; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return refuse(line_out, lineno, err, "a stage number is decimal digits only");
        }
        v = v * 10 + (*p - '0');
        if (v > 1000) {
            return refuse(line_out, lineno, err, "a stage number is at most %d",
                          ATLAS_PLAN_MAX_STAGES);
        }
    }
    if (v != c->stage_no + 1) {
        return refuse(line_out, lineno, err,
                      "stages are numbered from 1 upwards with no gaps; this plan is at stage %d "
                      "and this line says %d",
                      c->stage_no, v);
    }
    if (v > ATLAS_PLAN_MAX_STAGES) {
        return refuse(line_out, lineno, err, "a plan has at most %d stages", ATLAS_PLAN_MAX_STAGES);
    }
    atlas_status st = close_task(c, line_out, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (c->stage_no >= 1 && c->tree_in_stage != 1) {
        return refuse(line_out, c->stage_open_line, err,
                      "stage %d declares %d tree tasks; each stage declares exactly one",
                      c->stage_no, c->tree_in_stage);
    }
    c->stage_no = v;
    c->stage_open_line = lineno;
    c->tree_in_stage = 0;
    c->side_in_stage = 0;
    c->doc.stage_count = v;
    return ATLAS_OK;
}

static atlas_status parse_task(plan_parse *c, const char *rest, int lineno, int *line_out,
                               atlas_err *err) {
    if (c->stage_no == 0) {
        return refuse(line_out, lineno, err, "a task: line must follow a stage: line");
    }
    atlas_status st = close_task(c, line_out, err);
    if (st != ATLAS_OK) {
        return st;
    }
    size_t n = strlen(rest);
    if (!key_is_well_formed(rest, n)) {
        return refuse(line_out, lineno, err,
                      "a task key is 1 to 32 characters of [a-z0-9-]; this one is not");
    }
    if (c->doc.task_count >= ATLAS_PLAN_MAX_TASKS) {
        return refuse(line_out, lineno, err, "a plan has at most %d tasks in total",
                      ATLAS_PLAN_MAX_TASKS);
    }
    for (size_t i = 0; i < c->doc.task_count; i++) {
        if (strcmp(c->doc.tasks[i].key, rest) == 0) {
            /* The key is printable by construction — it passed the charset
             * check above — so quoting it back is safe. */
            return refuse(line_out, lineno, err, "task key %s is used more than once in this plan",
                          rest);
        }
    }
    size_t idx = c->doc.task_count++;
    memcpy(c->doc.tasks[idx].key, rest, n);
    c->doc.tasks[idx].key[n] = '\0';
    c->doc.tasks[idx].stage_no = c->stage_no;
    c->cur = (int)idx;
    c->cur_line = lineno;
    c->have_kind = false;
    c->have_title = false;
    c->have_prompt = false;
    return ATLAS_OK;
}

static atlas_status parse_kind(plan_parse *c, const char *rest, int lineno, int *line_out,
                               atlas_err *err) {
    if (c->cur < 0) {
        return refuse(line_out, lineno, err, "a kind: line must follow a task: line");
    }
    if (c->have_kind) {
        /* Refused rather than treated as an overwrite. `kind: tree`, a gate,
         * then `kind: side` would otherwise smuggle a gate onto a side task
         * past the check below — the one refusal in this file whose absence
         * would be a hole rather than an inconvenience. */
        return refuse(line_out, lineno, err, "task %s declares kind: more than once",
                      c->doc.tasks[c->cur].key);
    }
    if (strcmp(rest, "tree") == 0) {
        if (c->tree_in_stage >= 1) {
            return refuse(line_out, lineno, err,
                          "stage %d already has a tree task; each stage declares exactly one",
                          c->stage_no);
        }
        c->tree_in_stage++;
        c->doc.tasks[c->cur].is_tree = true;
    } else if (strcmp(rest, "side") == 0) {
        if (c->side_in_stage >= c->max_side) {
            return refuse(line_out, lineno, err,
                          "stage %d may declare at most %d side tasks, which is the smaller of %d "
                          "and this plan's max_parallel minus one",
                          c->stage_no, c->max_side, ATLAS_PLAN_MAX_SIDE_PER_STAGE);
        }
        c->side_in_stage++;
        c->doc.tasks[c->cur].is_tree = false;
    } else {
        return refuse(line_out, lineno, err, "kind: is tree or side");
    }
    c->have_kind = true;
    return ATLAS_OK;
}

static atlas_status parse_title(plan_parse *c, const char *rest, int lineno, int *line_out,
                                atlas_err *err) {
    if (c->cur < 0) {
        return refuse(line_out, lineno, err, "a title: line must follow a task: line");
    }
    if (c->have_title) {
        return refuse(line_out, lineno, err, "task %s declares title: more than once",
                      c->doc.tasks[c->cur].key);
    }
    size_t n = strlen(rest);
    if (n == 0) {
        return refuse(line_out, lineno, err, "a task needs a title");
    }
    if (n > (size_t)ATLAS_PLAN_TITLE_MAX) {
        return refuse(line_out, lineno, err, "a title is at most %d bytes; this one is %zu",
                      ATLAS_PLAN_TITLE_MAX, n);
    }
    memcpy(c->doc.tasks[c->cur].title, rest, n);
    c->doc.tasks[c->cur].title[n] = '\0';
    c->have_title = true;
    return ATLAS_OK;
}

static atlas_status parse_gate(plan_parse *c, const char *rest, int lineno, int *line_out,
                               atlas_err *err) {
    if (c->cur < 0) {
        return refuse(line_out, lineno, err, "a gate: line must follow a task: line");
    }
    if (!c->have_kind) {
        return refuse(line_out, lineno, err,
                      "a gate: line must follow this task's kind: tree line");
    }
    if (!c->doc.tasks[c->cur].is_tree) {
        return refuse(line_out, lineno, err,
                      "only a tree task may declare a gate; task %s is a side task",
                      c->doc.tasks[c->cur].key);
    }
    atlas_plan_doc_task *t = &c->doc.tasks[c->cur];
    if (t->gate_count >= ATLAS_ORCH_MAX_VALIDATIONS) {
        return refuse(line_out, lineno, err, "a task may add at most %u gates",
                      (unsigned)ATLAS_ORCH_MAX_VALIDATIONS);
    }
    atlas_orch_argv *v = &t->gates[t->gate_count];
    /* The operator's `--gate` splitter, called rather than reimplemented. Its
     * own refusals — an empty command, a non-printable byte, an over-long
     * argument, too many arguments — are already the sentences an operator
     * would get, so they are passed through with the line number attached. */
    atlas_status st = atlas_orch_gate_split(rest, v, err);
    if (st != ATLAS_OK) {
        if (line_out != NULL) {
            *line_out = lineno;
        }
        return st;
    }
    const char *prog = atlas_buf_cstr(&v->args[0]);
    if (!atlas_validation_program_allowed(prog)) {
        /* `prog` is printable ASCII by construction: `atlas_orch_argv_push`
         * refused anything else before this line was reached. */
        return refuse(line_out, lineno, err, "the gate program %s is not on the allowlist", prog);
    }
    t->gate_count++;
    return ATLAS_OK;
}

/* One line of a prompt heredoc: its own bytes and one newline. The terminator
 * is the line `>>` exactly — `>> ` with a trailing space is content, because a
 * terminator that tolerates trailing bytes is a terminator a body line can
 * accidentally be. */
static atlas_status parse_heredoc_line(plan_parse *c, const char *line, size_t n, int lineno,
                                       int *line_out, atlas_err *err) {
    if (n == 2u && line[0] == '>' && line[1] == '>') {
        c->in_heredoc = false;
        c->have_prompt = true;
        return ATLAS_OK;
    }
    atlas_buf *p = &c->doc.tasks[c->cur].prompt;
    if (p->len + n + 1u > (size_t)ATLAS_PLAN_TASK_PROMPT_MAX) {
        return refuse(line_out, lineno, err, "a prompt is at most %d bytes",
                      ATLAS_PLAN_TASK_PROMPT_MAX);
    }
    atlas_status st = atlas_buf_append(p, line, n, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_append_ch(p, '\n', err);
    }
    return st;
}

static atlas_status parse_line(plan_parse *c, const char *line, size_t n, int lineno,
                               int *line_out, atlas_err *err) {
    if (c->in_heredoc) {
        return parse_heredoc_line(c, line, n, lineno, line_out, err);
    }
    if (lineno == 1) {
        if (strcmp(line, "atlas-plan-1") != 0) {
            return refuse(line_out, 1, err,
                          "the first line of a plan is exactly atlas-plan-1");
        }
        return ATLAS_OK;
    }
    const char *rest = NULL;
    if (eat(line, "stage: ", &rest)) {
        return parse_stage(c, rest, lineno, line_out, err);
    }
    if (eat(line, "task: ", &rest)) {
        return parse_task(c, rest, lineno, line_out, err);
    }
    if (eat(line, "kind: ", &rest)) {
        return parse_kind(c, rest, lineno, line_out, err);
    }
    if (eat(line, "title: ", &rest)) {
        return parse_title(c, rest, lineno, line_out, err);
    }
    if (eat(line, "gate: ", &rest)) {
        return parse_gate(c, rest, lineno, line_out, err);
    }
    if (strcmp(line, "prompt<<") == 0) {
        if (c->cur < 0) {
            return refuse(line_out, lineno, err, "a prompt<< block must follow a task: line");
        }
        if (c->have_prompt) {
            return refuse(line_out, lineno, err, "task %s declares prompt<< more than once",
                          c->doc.tasks[c->cur].key);
        }
        c->in_heredoc = true;
        c->heredoc_line = lineno;
        return ATLAS_OK;
    }
    /* Including a blank line. A plan is a document Atlas compiles into jobs, so
     * a line nobody can account for is a refusal rather than something skipped:
     * skipping it would make a plan whose meaning depends on what the parser
     * happened not to understand. */
    return refuse(line_out, lineno, err, "this line is not part of the atlas-plan-1 format");
}

static atlas_status parse_finish(plan_parse *c, int *line_out, atlas_err *err) {
    if (c->in_heredoc) {
        return refuse(line_out, c->heredoc_line, err,
                      "the prompt<< block opened here is never terminated by a >> line");
    }
    atlas_status st = close_task(c, line_out, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (c->stage_no == 0) {
        return refuse(line_out, 0, err, "a plan declares at least one stage");
    }
    if (c->tree_in_stage != 1) {
        return refuse(line_out, c->stage_open_line, err,
                      "stage %d declares %d tree tasks; each stage declares exactly one",
                      c->stage_no, c->tree_in_stage);
    }
    return ATLAS_OK;
}

atlas_status atlas_plan_parse(const void *bytes, size_t len, int max_parallel, atlas_plan_doc *out,
                              int *line_out, atlas_err *err) {
    if (line_out != NULL) {
        *line_out = 0;
    }
    if (out == NULL) {
        return refuse(line_out, 0, err, "a parsed plan needs somewhere to go");
    }
    if (max_parallel < 1) {
        /* Refused, never resolved to a default. A pure function that invents
         * one is a pure function whose answer depends on something its caller
         * cannot see. */
        return refuse(line_out, 0, err, "a plan's max_parallel is at least 1; %d was given",
                      max_parallel);
    }
    if (bytes == NULL || len == 0) {
        return refuse(line_out, 0, err, "an empty document is not a plan");
    }
    if (len > (size_t)ATLAS_PLAN_MAX_BYTES) {
        return refuse(line_out, 0, err, "a plan document is at most %d bytes; this one is %zu",
                      ATLAS_PLAN_MAX_BYTES, len);
    }

    /* About 50 KiB of automatic storage: eight tasks, each holding eight argv
     * vectors of thirty-two buffers. Sized rather than allocated because the
     * bound is compiled in and a fixed shape cannot be exhausted by input. */
    plan_parse c;
    memset(&c, 0, sizeof c);
    c.cur = -1;
    c.max_side = side_bound(max_parallel);

    const char *p = (const char *)bytes;
    char line[ATLAS_PLAN_MAX_LINE + 1];
    atlas_status st = ATLAS_OK;
    size_t off = 0;
    int lineno = 0;
    while (st == ATLAS_OK && off < len) {
        const char *nl = (const char *)memchr(p + off, '\n', len - off);
        size_t raw = nl != NULL ? (size_t)(nl - (p + off)) : len - off;
        lineno++;
        if (memchr(p + off, '\0', raw) != NULL) {
            /* Everywhere, including inside a heredoc. A NUL is the one byte a
             * prompt may not hold: it is the delimiter every C consumer of that
             * prompt would stop at, so a document containing one describes a
             * different task to Atlas than to the process it reaches. */
            st = refuse(line_out, lineno, err, "this line contains a NUL byte");
            break;
        }
        if (raw > (size_t)ATLAS_PLAN_MAX_LINE) {
            st = refuse(line_out, lineno, err, "a line is at most %d bytes; this one is %zu",
                        ATLAS_PLAN_MAX_LINE, raw);
            break;
        }
        /* One trailing carriage return, so a document written on another
         * platform parses. Exactly one: a line ending `\r\r` keeps the first,
         * because the second is content by the same argument the `>>`
         * terminator is exact. */
        size_t n = raw;
        if (n > 0 && (p + off)[n - 1] == '\r') {
            n--;
        }
        memcpy(line, p + off, n);
        line[n] = '\0';
        st = parse_line(&c, line, n, lineno, line_out, err);
        off += raw + (nl != NULL ? 1u : 0u);
    }
    if (st == ATLAS_OK) {
        st = parse_finish(&c, line_out, err);
    }
    if (st == ATLAS_OK) {
        /* Ownership moves whole, once, at the end. A refused parse never
         * touches `out`, so a caller cannot read half a plan out of it. */
        *out = c.doc;
        memset(&c.doc, 0, sizeof c.doc);
    }
    atlas_plan_doc_free(&c.doc);
    return st;
}

/* --- the prompts ---------------------------------------------------------- */

/* The format specification the planner is shown, and the sentences the parser
 * above enforces. Numbers rather than macro names, pinned by the
 * `_Static_assert`s at the top of this file. */
static const char PLAN_FORMAT_SPEC[] =
    "plan-format: atlas-plan-1\n"
    "\n"
    "The document is line-based. Lines are at most 4096 bytes. One trailing\n"
    "carriage return per line is stripped. UTF-8 is not assumed. Any line the\n"
    "parser does not recognise is a refusal naming the line number.\n"
    "\n"
    "atlas-plan-1\n"
    "stage: 1\n"
    "task: <key>            # [a-z0-9-]{1,32}, unique across the whole plan\n"
    "kind: tree             # exactly one tree task per stage\n"
    "title: <one line, at most 200 bytes>\n"
    "gate: <cmd>            # tree only, 0..n; appended AFTER the operator floor;\n"
    "                       # same parsing as --gate (space-split argv, allowlist\n"
    "                       # make/ctest/cmake/true/false)\n"
    "prompt<<\n"
    "<free text for the executor, at most 16384 bytes>\n"
    ">>\n"
    "task: <key2>\n"
    "kind: side             # 0..3 per stage; no gate: lines allowed on side tasks\n"
    "title: ...\n"
    "prompt<<\n"
    "...\n"
    ">>\n"
    "stage: 2\n"
    "...\n"
    "\n"
    "Every refusal names what and where:\n"
    "- the header line is exactly `atlas-plan-1`;\n"
    "- stages are numbered 1..N ascending with no gaps, N at most 4, and there is\n"
    "  at least one;\n"
    "- exactly one `kind: tree` per stage; side tasks per stage at most 3 and at\n"
    "  most (max_parallel - 1), which for this plan is the bound stated above;\n"
    "- at most 8 tasks in total; keys unique across the plan; every task carries\n"
    "  all four of `task`, `kind`, `title` and `prompt`;\n"
    "- `gate:` under `kind: side` is a refusal, and so is a `gate:` line this\n"
    "  task's `kind: tree` line has not yet been reached;\n"
    "- a gate program outside the allowlist is a refusal; a tree task may add at\n"
    "  most 8 gates, and the operator floor is prepended to them, never replaced\n"
    "  by them;\n"
    "- `title:`, `gate:` and `prompt<<` before this task's `task:` line are\n"
    "  refusals;\n"
    "- a repeated `kind:`, `title:` or `prompt<<` inside one task is a refusal;\n"
    "- a field line's prefix is exactly `stage: `, `task: `, `kind: `, `title: `\n"
    "  or `gate: `, and a heredoc opens on the whole line `prompt<<`;\n"
    "- outside a heredoc, a blank line is a refusal, as is any other\n"
    "  unrecognised line;\n"
    "- the heredoc terminator is the line `>>` exactly: `>> ` with a trailing\n"
    "  space is prompt content, and a heredoc that is never terminated is a\n"
    "  refusal;\n"
    "- a prompt is required and is at most 16384 bytes; each body line\n"
    "  contributes its own bytes and one newline;\n"
    "- a NUL byte anywhere in the document is a refusal;\n"
    "- the whole document is at most 65536 bytes.\n";

/* The constraints every one of the five prompts ends with.
 *
 * All three sentences are true because of what is absent elsewhere — there is
 * no `plan.settle`, no RPC method that accepts a run, no way for a worker to
 * reach `atlas_db_orch_run_set_status` — and not because they are written here.
 * That is exactly why they can be stated to a model at all: they are a
 * description of the arrangement, not the arrangement. */
static const char PLAN_CONSTRAINTS[] =
    "- This is instruction, not enforcement. Nothing you write changes what Atlas\n"
    "  does or what it will accept.\n"
    "- Atlas runs the verification gates itself and settles the run itself. Your\n"
    "  output grants nothing: it is not an approval, an acceptance or a verdict.\n"
    "- Report honestly. Saying that the work is done is not a decision that it is.\n";

static const char PLAN_SCOPE_TREE[] =
    "- Work only inside the registered repository's own root.\n"
    "- Apply exactly this task. Do not change its scope, the plan, or any\n"
    "  verification gate.\n"
    "- Do not commit, push, deploy, restart a daemon, or run any destructive\n"
    "  git operation. Leave the working tree as you found it plus your\n"
    "  changes.\n";

static const char PLAN_SCOPE_SIDE[] =
    "- Work only inside the workspace you were given. Produce your results as\n"
    "  files under artifacts/.\n"
    "- Apply exactly this task. Do not change its scope, the plan, or any\n"
    "  verification gate.\n"
    "- You cannot modify the repository itself and must not attempt to.\n";

/* `src/db/db_orch.c`'s `append_bounded`, mirrored: truncation announces itself
 * rather than happening quietly. An excerpt that silently stops is an excerpt a
 * reader believes is the whole thing. */
static atlas_status append_bounded(atlas_buf *out, const char *text, size_t len, size_t cap,
                                   atlas_err *err) {
    if (text == NULL) {
        return ATLAS_OK;
    }
    size_t n = len < cap ? len : cap;
    atlas_status s = atlas_buf_append(out, text, n, err);
    if (s == ATLAS_OK && n < len) {
        s = atlas_buf_appendf(out, err, "\n[... %zu further bytes not shown ...]\n", len - n);
    }
    return s;
}

/* Keeps a section on its own lines whatever the appended block ended with. A
 * goal, a floor or a prompt is somebody else's bytes and may or may not end in
 * a newline; the surrounding document's shape may not depend on which. */
static atlas_status ensure_nl(atlas_buf *out, atlas_err *err) {
    if (out->len > 0 && out->data[out->len - 1] == '\n') {
        return ATLAS_OK;
    }
    return atlas_buf_append_ch(out, '\n', err);
}

/* The head every planner-facing prompt opens with: what is wanted, the
 * operator's goal, the floor that may not move, and the bounds. */
static atlas_status compose_head(const char *goal, const char *gate_floor_text, int max_parallel,
                                 atlas_buf *out, atlas_err *err) {
    if (goal == NULL || goal[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a plan needs a goal");
    }
    size_t goal_len = strlen(goal);
    if (goal_len > (size_t)ATLAS_PLAN_GOAL_MAX) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a goal is at most %d bytes; this one is %zu",
                             ATLAS_PLAN_GOAL_MAX, goal_len);
    }
    if (gate_floor_text == NULL || gate_floor_text[0] == '\0') {
        /* The operator brings the gate floor. A plan with none could only ever
         * be accepted on a model's word, which is the one thing this season
         * exists to keep impossible. */
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a plan needs at least one operator gate");
    }
    if (max_parallel < 1) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a plan's max_parallel is at least 1; %d was "
                                                   "given",
                             max_parallel);
    }
    atlas_status s = atlas_buf_set_str(out,
                                       "atlas-plan-request: produce a plan for the goal below.\n"
                                       "\n"
                                       "goal:\n",
                                       err);
    if (s == ATLAS_OK) {
        /* Raw bytes. The worker receives the operator's own words under the
         * existing lease contract; the label belongs on the read-back surfaces,
         * where the goal is somebody else's text. */
        s = atlas_buf_append(out, goal, goal_len, err);
    }
    if (s == ATLAS_OK) {
        s = ensure_nl(out, err);
    }
    if (s == ATLAS_OK) {
        s = atlas_buf_append_str(out, "\ngate-floor (immutable, operator-supplied):\n", err);
    }
    if (s == ATLAS_OK) {
        s = atlas_buf_append_str(out, gate_floor_text, err);
    }
    if (s == ATLAS_OK) {
        s = ensure_nl(out, err);
    }
    if (s == ATLAS_OK) {
        s = atlas_buf_appendf(out, err,
                              "\nbounds:\n"
                              "- stages: at most %d\n"
                              "- tasks in total: at most %d\n"
                              "- side tasks per stage: at most %d\n"
                              "\n",
                              ATLAS_PLAN_MAX_STAGES, ATLAS_PLAN_MAX_TASKS,
                              side_bound(max_parallel));
    }
    return s;
}

/* What must be produced, and the frozen format it must be produced in. */
static atlas_status compose_required(atlas_buf *out, atlas_err *err) {
    atlas_status s = atlas_buf_appendf(out, err,
                                       "required-output:\n"
                                       "Write exactly one file, artifacts/%s, in the format\n"
                                       "atlas-plan-1 specified below. Write no other plan, in no "
                                       "other place, in no\nother format.\n"
                                       "\n",
                                       ATLAS_PLAN_ARTIFACT_NAME);
    if (s == ATLAS_OK) {
        s = atlas_buf_append_str(out, PLAN_FORMAT_SPEC, err);
    }
    return s;
}

/* The constraints block, last in every form. `scope` is NULL for the planner
 * prompts and one of the two scope locks for an executor's. */
static atlas_status compose_constraints(atlas_buf *out, const char *scope, atlas_err *err) {
    atlas_status s = atlas_buf_append_str(out, "\nconstraints:\n", err);
    if (s == ATLAS_OK && scope != NULL) {
        s = atlas_buf_append_str(out, scope, err);
    }
    if (s == ATLAS_OK) {
        s = atlas_buf_append_str(out, PLAN_CONSTRAINTS, err);
    }
    return s;
}

atlas_status atlas_plan_compose_planner(const char *goal, const char *gate_floor_text,
                                        int max_parallel, atlas_buf *out, atlas_err *err) {
    atlas_status s = compose_head(goal, gate_floor_text, max_parallel, out, err);
    if (s == ATLAS_OK) {
        s = compose_required(out, err);
    }
    if (s == ATLAS_OK) {
        s = compose_constraints(out, NULL, err);
    }
    return s;
}

atlas_status atlas_plan_compose_planner_retry(const char *goal, const char *gate_floor_text,
                                              int max_parallel, const char *refusal,
                                              const void *refused, size_t refused_len,
                                              atlas_buf *out, atlas_err *err) {
    if (refusal == NULL || refusal[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a retry prompt needs the refusal it answers");
    }
    atlas_status s = compose_head(goal, gate_floor_text, max_parallel, out, err);
    if (s == ATLAS_OK) {
        /* The refusal is Atlas' own sentence, from `atlas_plan_parse`. The
         * excerpt beside it is the planner's bytes, bounded and labelled. */
        s = atlas_buf_appendf(out, err,
                              "previous-plan-refused: %s\n"
                              "refused-excerpt (bounded, untrusted):\n",
                              refusal);
    }
    if (s == ATLAS_OK) {
        s = append_bounded(out, (const char *)refused, refused_len, ATLAS_ORCH_GATE_EXCERPT_MAX,
                           err);
    }
    if (s == ATLAS_OK) {
        s = ensure_nl(out, err);
    }
    if (s == ATLAS_OK) {
        s = atlas_buf_append_ch(out, '\n', err);
    }
    if (s == ATLAS_OK) {
        s = compose_required(out, err);
    }
    if (s == ATLAS_OK) {
        s = compose_constraints(out, NULL, err);
    }
    return s;
}

atlas_status atlas_plan_compose_replan(const char *goal, const char *gate_floor_text,
                                       int max_parallel, const atlas_plan_state *st,
                                       const char *blocked_key, const char *failed_gate,
                                       const void *gate_excerpt, size_t excerpt_len,
                                       atlas_buf *out, atlas_err *err) {
    if (st == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a replan prompt needs the plan's state");
    }
    if (blocked_key == NULL || blocked_key[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a replan prompt needs the blocked task");
    }
    atlas_status s = compose_head(goal, gate_floor_text, max_parallel, out, err);
    if (s == ATLAS_OK) {
        s = atlas_buf_append_str(out, "completed-work (Atlas facts):\n", err);
    }
    /* What Atlas established, in the order the tasks are held. A task's title
     * is the planner's own words and is labelled as such even here, where the
     * verdict beside it is Atlas'. */
    int shown = 0;
    for (int i = 0; s == ATLAS_OK && i < st->task_count && i < ATLAS_PLAN_MAX_TASKS; i++) {
        if (st->tasks[i].job_state != ATLAS_ORCH_STATE_SUCCEEDED) {
            continue;
        }
        shown++;
        s = atlas_buf_appendf(out, err, "- stage %d task ", st->tasks[i].stage_no);
        if (s == ATLAS_OK) {
            s = append_bounded(out, st->tasks[i].task_key, strlen(st->tasks[i].task_key), 32u, err);
        }
        if (s == ATLAS_OK) {
            s = atlas_buf_append_str(out, ": SUCCEEDED  title (untrusted): ", err);
        }
        if (s == ATLAS_OK) {
            s = append_bounded(out, st->tasks[i].title, strlen(st->tasks[i].title),
                               (size_t)ATLAS_PLAN_TITLE_MAX, err);
        }
        if (s == ATLAS_OK) {
            s = atlas_buf_append_ch(out, '\n', err);
        }
    }
    if (s == ATLAS_OK && shown == 0) {
        /* Stated rather than left blank: "nothing succeeded" and "nobody wrote
         * this section" are different claims. */
        s = atlas_buf_append_str(out, "- (none)\n", err);
    }
    if (s == ATLAS_OK) {
        s = atlas_buf_append_str(out, "\nblocked-task: ", err);
    }
    if (s == ATLAS_OK) {
        s = append_bounded(out, blocked_key, strlen(blocked_key), 32u, err);
    }
    if (s == ATLAS_OK) {
        s = atlas_buf_append_str(out, "\nfailed-gate: ", err);
    }
    if (s == ATLAS_OK) {
        /* The gate's name comes from the job's own stored validations, by
         * index. A caller that has none says so rather than naming one. */
        if (failed_gate != NULL && failed_gate[0] != '\0') {
            s = append_bounded(out, failed_gate, strlen(failed_gate), 256u, err);
        } else {
            s = atlas_buf_append_str(out, "(none recorded)", err);
        }
    }
    if (s == ATLAS_OK) {
        s = atlas_buf_append_ch(out, '\n', err);
    }
    if (s == ATLAS_OK && gate_excerpt != NULL && excerpt_len > 0) {
        s = atlas_buf_append_str(out, "gate-output (bounded excerpt, untrusted):\n", err);
        if (s == ATLAS_OK) {
            s = append_bounded(out, (const char *)gate_excerpt, excerpt_len,
                               ATLAS_ORCH_GATE_EXCERPT_MAX, err);
        }
        if (s == ATLAS_OK) {
            s = ensure_nl(out, err);
        }
    }
    if (s == ATLAS_OK) {
        s = atlas_buf_append_str(
            out,
            "\ninstruction:\n"
            "Produce a complete new plan, in the format below, for the REMAINING work\n"
            "only. The completed work above stands: do not redo it, do not undo it and\n"
            "do not plan it again. You may add gates; you may not remove, replace or\n"
            "weaken one the operator set.\n"
            "\n",
            err);
    }
    if (s == ATLAS_OK) {
        s = compose_required(out, err);
    }
    if (s == ATLAS_OK) {
        s = compose_constraints(out, NULL, err);
    }
    return s;
}

atlas_status atlas_plan_compose_executor(const char *plan_uid, int rev_no,
                                         const atlas_plan_doc_task *t, atlas_buf *out,
                                         atlas_err *err) {
    if (t == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "an executor prompt needs a task");
    }
    if (plan_uid == NULL || plan_uid[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "an executor prompt needs the plan it belongs "
                                                   "to");
    }
    atlas_status s = atlas_buf_set_str(out, "atlas-plan-task:\n", err);
    if (s == ATLAS_OK) {
        s = atlas_buf_appendf(out, err,
                              "plan: %s\n"
                              "revision: %d\n"
                              "stage: %d\n"
                              "task: %s\n"
                              "title (untrusted): ",
                              plan_uid, rev_no, t->stage_no, t->key);
    }
    if (s == ATLAS_OK) {
        s = append_bounded(out, t->title, strlen(t->title), (size_t)ATLAS_PLAN_TITLE_MAX, err);
    }
    if (s == ATLAS_OK) {
        s = atlas_buf_append_str(out, "\n\ninstructions (untrusted, from the plan):\n", err);
    }
    if (s == ATLAS_OK) {
        /* The planner's own bytes, whole and unmodified within the bound the
         * parser already applied. Never quoted, never escaped, never merged
         * into a sentence of Atlas': the label above it is the boundary, and a
         * label is honest only when what follows it is unaltered. */
        s = append_bounded(out, t->prompt.data, t->prompt.len,
                           (size_t)ATLAS_PLAN_TASK_PROMPT_MAX, err);
    }
    if (s == ATLAS_OK) {
        s = ensure_nl(out, err);
    }
    if (s == ATLAS_OK) {
        s = compose_constraints(out, t->is_tree ? PLAN_SCOPE_TREE : PLAN_SCOPE_SIDE, err);
    }
    return s;
}

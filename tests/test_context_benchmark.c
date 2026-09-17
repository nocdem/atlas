/* Atlas - reproducible context-profile measurements through the real CLI.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * CSV goes to stdout; fixture assertions go to stderr. No model is invoked.
 * Timing includes process startup, local database reads and JSON rendering;
 * fixture creation and indexing are outside the timed region.
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "atlas/jsonread.h"
#include "atlas/sem.h"
#include "atlas_test.h"
#include "support/fixture.h"

#define SAMPLES 11
#define WORKERS 80

static void run(fixture *fx, const char *const *args, size_t count, atlas_buf *out,
                atlas_err *err) {
    const char *argv[24] = {"--data-dir", fx_data_dir(fx), "--json"};
    T_REQUIRE(count + 3u <= sizeof argv / sizeof argv[0]);
    for (size_t i = 0; i < count; i++) argv[i + 3u] = args[i];
    atlas_buf errors = ATLAS_BUF_INIT;
    int code = -1;
    T_OK(fx_atlas(argv, count + 3u, out, &errors, &code, err), err);
    T_REQUIRE_MSG(code == 0, "CLI exited %d: %s", code, atlas_buf_cstr(&errors));
    atlas_buf_free(&errors);
}

static void seed(fixture *fx, atlas_err *err) {
    T_OK(fx_open(fx, err), err);
    T_OK(fx_init_repo(fx, fx_repo(fx), NULL, err), err);
    T_OK(fx_write(fx_repo(fx), "core.h", "int shared_core(int value);\n", err), err);
    T_OK(fx_write(fx_repo(fx), "core.c",
                  "#include \"core.h\"\nint shared_core(int value) { return value + 1; }\n",
                  err), err);
    T_OK(fx_write(fx_repo(fx), "single.c", "int alpha_unique(void) { return 7; }\n", err), err);
    atlas_buf source = ATLAS_BUF_INIT;
    T_OK(atlas_buf_append_str(&source, "#include \"core.h\"\n", err), err);
    for (unsigned i = 0; i < WORKERS; i++) {
        T_OK(atlas_buf_appendf(&source, err,
              "int work_worker_%02u(int value) { return shared_core(value); }\n", i), err);
    }
    T_OK(fx_write(fx_repo(fx), "workers.c", atlas_buf_cstr(&source), err), err);
    atlas_buf_free(&source);
    const char *files[] = {"core.c", "single.c", "workers.c"};
    atlas_buf compdb = ATLAS_BUF_INIT;
    T_OK(atlas_buf_append_str(&compdb, "[", err), err);
    for (size_t i = 0; i < 3; i++) {
        T_OK(atlas_buf_appendf(&compdb, err,
              "%s{\"directory\":\"%s\",\"file\":\"%s\","
              "\"arguments\":[\"cc\",\"-std=c17\",\"-c\",\"%s\"]}",
              i == 0 ? "" : ",", fx_repo(fx), files[i], files[i]), err);
    }
    T_OK(atlas_buf_append_str(&compdb, "]", err), err);
    T_OK(fx_write(fx_repo(fx), "compile_commands.json", atlas_buf_cstr(&compdb), err), err);
    atlas_buf_free(&compdb);
    T_OK(fx_add_all(fx, fx_repo(fx), err), err);
    T_OK(fx_commit(fx, fx_repo(fx), "context benchmark fixture v1", err), err);

    atlas_buf out = ATLAS_BUF_INIT;
    const char *add[] = {"repo", "add", fx_repo(fx), "--name", "fixture"};
    const char *scan[] = {"scan", "fixture"};
    const char *cfg[] = {"code", "sem-config", "fixture", "--compdb",
                         "compile_commands.json", "--auto"};
    const char *index[] = {"code", "index", "fixture"};
    run(fx, add, 5, &out, err);
    run(fx, scan, 2, &out, err);
    run(fx, cfg, 6, &out, err);
    run(fx, index, 3, &out, err);
    T_REQUIRE(strstr(atlas_buf_cstr(&out), "\"published\":true") != NULL);
    atlas_buf_free(&out);
}

static int64_t now_us(void) {
    struct timespec t;
    T_REQUIRE(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static int compare_time(const void *a, const void *b) {
    int64_t av = *(const int64_t *)a, bv = *(const int64_t *)b;
    return (av > bv) - (av < bv);
}

typedef struct task_case {
    const char *category;
    const char *task;
    const char *required_name;
    bool broad;
    bool missing;
} task_case;

typedef struct measurement {
    int64_t times[SAMPLES];
    size_t items, wire_bytes;
    int64_t used_bytes;
    bool omitted;
} measurement;

static void check_result(const atlas_buf *out, const task_case *task, bool compact,
                         measurement *m, atlas_err *err) {
    atlas_jsondoc *doc = NULL;
    T_OK(atlas_jsondoc_parse(out->data, out->len, 1024u * 1024u, 32, &doc, err), err);
    const atlas_jsonv *root = atlas_jsondoc_root(doc);
    const atlas_jsonv *items = atlas_jsonv_get(root, "items");
    const atlas_jsonv *missing = atlas_jsonv_get(root, "not_included");
    T_REQUIRE(atlas_jsonv_is_arr(items));
    T_REQUIRE(atlas_jsonv_is_arr(missing));
    m->items = atlas_jsonv_arr_len(items);
    m->wire_bytes = out->len;
    int64_t budget = 0;
    T_REQUIRE(atlas_jsonv_int(atlas_jsonv_get(root, "used_bytes"), &m->used_bytes));
    T_REQUIRE(atlas_jsonv_int(atlas_jsonv_get(root, "budget_bytes"), &budget));
    T_REQUIRE(atlas_jsonv_bool(atlas_jsonv_get(root, "budget_reached"), &m->omitted));
    T_EQ_INT(budget, compact ? ATLAS_SEM_CONTEXT_DEFAULT_BYTES : 32768);
    T_CHECK(m->used_bytes <= budget);
    T_CHECK(m->items <= (compact ? ATLAS_SEM_CONTEXT_DEFAULT_ITEMS : 400));
    bool found = task->required_name == NULL;
    for (size_t i = 0; i < m->items; i++) {
        const atlas_jsonv *item = atlas_jsonv_at(items, i);
        const char *name = NULL, *why = NULL, *evidence = NULL;
        T_REQUIRE(atlas_jsonv_str(atlas_jsonv_get(item, "name"), &name, NULL));
        T_REQUIRE(atlas_jsonv_str(atlas_jsonv_get(item, "why"), &why, NULL));
        T_REQUIRE(atlas_jsonv_str(atlas_jsonv_get(item, "evidence"), &evidence, NULL));
        T_CHECK(atlas_sem_selection_reason_is_known(why));
        T_CHECK(evidence[0] != '\0');
        if (task->required_name != NULL && strcmp(name, task->required_name) == 0) found = true;
    }
    T_CHECK_MSG(found, "%s lost the required symbol %s", task->category, task->required_name);
    if (task->broad) {
        T_CHECK(m->items >= (compact ? 20u : WORKERS));
        if (compact) T_CHECK(m->omitted);
    }
    if (m->omitted || task->missing) T_CHECK(atlas_jsonv_arr_len(missing) > 0);
    if (task->missing) T_EQ_INT(m->items, 0);
    atlas_jsondoc_free(doc);
}

static void benchmark(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    seed(&fx, &err);
    static const task_case tasks[] = {
        {"known-symbol", "explain alpha_unique", "alpha_unique", false, false},
        {"cross-file", "change shared_core", "shared_core", true, false},
        {"broad-handoff", "review shared_core alpha_unique work_worker_00", "work_worker_00", true, false},
        {"prefix-recall", "review work_worker", "work_worker_00", false, false},
        {"path-recall", "inspect core.c", "shared_core", true, false},
        {"missing-evidence", "nonexistentzz", NULL, false, true},
    };
    (void)puts("category,profile,samples,p50_us,p95_us,items,used_bytes,wire_bytes,budget_reached");
    for (size_t t = 0; t < sizeof tasks / sizeof tasks[0]; t++) {
        measurement m[2];
        memset(m, 0, sizeof m);
        /* One warmup per profile, then alternating order avoids systematically
         * giving one profile the warmer cache. Eleven samples; p95 is max. */
        for (int sample = -1; sample < SAMPLES; sample++) {
            for (unsigned order = 0; order < 2; order++) {
                unsigned p = (order + (unsigned)(sample + 1)) % 2u;
                bool compact = p == 1u;
                const char *args[] = {"context", "build", "--repo", "fixture", "--task",
                    tasks[t].task, "--limit", compact ? "24" : "400", "--max-tokens",
                    compact ? "2048" : "8192"};
                atlas_buf out = ATLAS_BUF_INIT;
                int64_t start = now_us();
                /* The compact profile uses actual defaults, not copied flags. */
                run(&fx, args, compact ? 6u : 10u, &out, &err);
                int64_t elapsed = now_us() - start;
                check_result(&out, &tasks[t], compact, &m[p], &err);
                if (sample >= 0) m[p].times[sample] = elapsed;
                atlas_buf_free(&out);
            }
        }
        if (tasks[t].broad) T_CHECK(m[1].wire_bytes < m[0].wire_bytes);
        for (unsigned p = 0; p < 2; p++) {
            qsort(m[p].times, SAMPLES, sizeof m[p].times[0], compare_time);
            (void)printf("%s,%s,%d,%lld,%lld,%zu,%lld,%zu,%s\n", tasks[t].category,
                p == 0 ? "previous-defaults" : "compact-defaults", SAMPLES,
                (long long)m[p].times[SAMPLES / 2], (long long)m[p].times[SAMPLES - 1],
                m[p].items, (long long)m[p].used_bytes, m[p].wire_bytes,
                m[p].omitted ? "true" : "false");
        }
    }
    fx_close(&fx);
}

static const atlas_test tests[] = {{"context profiles retain task evidence and disclose gaps", benchmark}};
int main(void) {
    if (!atlas_sem_available()) {
        (void)fputs("SKIP measurements: this build has no libclang\n", stderr);
        return 77;
    }
    return atlas_test_run("context benchmark", tests, sizeof tests / sizeof tests[0]);
}

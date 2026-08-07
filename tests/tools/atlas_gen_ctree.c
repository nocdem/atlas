/* Atlas - a deterministic synthetic C tree, for DNA-scale acceptance.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A0 refuses a build that needs Python, Node or any other runtime, and a
 * measurement fixture is part of the build. So this is a small C program: it
 * writes a tree of the shape a large C project has, from a fixed seed, with no
 * clock, no randomness the caller cannot reproduce, and no dependency beyond
 * libc.
 *
 * "Deterministic" is load-bearing rather than tidy. A performance number is only
 * comparable against another one if the input was identical, and a fixture that
 * differs run to run turns a regression into an argument.
 *
 * The tree is deliberately not uniform. A real repository has a common header
 * everything includes, files with the same `static` helper name, calls that go
 * nowhere, include cycles, and a test subtree — and every one of those is a case
 * the structural indexer resolves differently. A fixture of ten thousand
 * identical files would measure one code path and report it as the whole.
 *
 *   usage: atlas-gen-ctree DIR MODULES FILES_PER_MODULE
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

/* Entry functions per source file.
 *
 * Chosen so that the default fixture reaches the shape a large C project has —
 * roughly half a million lines, fifty thousand symbols and two hundred thousand
 * relations across five thousand files — rather than five thousand tiny files,
 * which would measure one code path and report it as the whole. */
#define ENTRIES_PER_FILE 5

/* A 64-bit linear congruential generator with fixed constants, seeded from the
 * file's own coordinates rather than from a running state. Every file's content
 * is therefore a pure function of (module, index) and does not depend on
 * generation order — so a partial regeneration produces the same bytes. */
static unsigned long long mix(unsigned long long a, unsigned long long b) {
    unsigned long long x = a * 6364136223846793005ULL + b * 1442695040888963407ULL;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return x;
}

static int make_dir(const char *path) {
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        (void)fprintf(stderr, "atlas-gen-ctree: cannot create %s: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

static FILE *open_file(const char *dir, const char *name) {
    /* Comfortably larger than the two components it joins, so the compiler can
     * see that the result cannot be truncated. A silently truncated path would
     * write the fixture somewhere other than where it was asked to. */
    char path[2304];
    int n = snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        (void)fprintf(stderr, "atlas-gen-ctree: path too long: %s/%s\n", dir, name);
        return NULL;
    }
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        (void)fprintf(stderr, "atlas-gen-ctree: cannot write %s: %s\n", path, strerror(errno));
    }
    return f;
}

/* The one header every module includes, so the fixture has the high-fan-in case
 * a real project has — and so a change to it exercises the reverse-dependency
 * path at full width. */
static int write_common(const char *root) {
    char dir[1024];
    (void)snprintf(dir, sizeof(dir), "%s/include", root);
    if (make_dir(dir) != 0) {
        return -1;
    }
    FILE *f = open_file(dir, "common.h");
    if (f == NULL) {
        return -1;
    }
    (void)fprintf(f, "#ifndef ATLAS_FIXTURE_COMMON_H\n#define ATLAS_FIXTURE_COMMON_H\n");
    (void)fprintf(f, "#include <stddef.h>\n#include <stdint.h>\n");
    (void)fprintf(f, "typedef struct fixture_ctx { int depth; long total; } fixture_ctx;\n");
    (void)fprintf(f, "enum fixture_mode { FIXTURE_FAST, FIXTURE_SLOW, FIXTURE_UNKNOWN };\n");
    (void)fprintf(f, "#define FIXTURE_MAX(a, b) ((a) > (b) ? (a) : (b))\n");
    (void)fprintf(f, "int fixture_common_helper(int value);\n");
    (void)fprintf(f, "int fixture_common_dispatch(fixture_ctx *ctx, int value);\n");
    (void)fprintf(f, "#endif\n");
    (void)fclose(f);

    f = open_file(dir, "common_impl.h");
    if (f == NULL) {
        return -1;
    }
    /* Two headers that include each other: an include cycle is ordinary in C
     * and a traversal that did not expect one would not terminate. */
    (void)fprintf(f, "#ifndef ATLAS_FIXTURE_COMMON_IMPL_H\n#define ATLAS_FIXTURE_COMMON_IMPL_H\n");
    (void)fprintf(f, "#include \"common_cycle.h\"\n");
    (void)fprintf(f, "int fixture_impl_detail(int value);\n#endif\n");
    (void)fclose(f);

    f = open_file(dir, "common_cycle.h");
    if (f == NULL) {
        return -1;
    }
    (void)fprintf(f, "#ifndef ATLAS_FIXTURE_COMMON_CYCLE_H\n#define ATLAS_FIXTURE_COMMON_CYCLE_H\n");
    (void)fprintf(f, "#include \"common_impl.h\"\n");
    (void)fprintf(f, "int fixture_cycle_detail(int value);\n#endif\n");
    (void)fclose(f);

    /* A second file called `config.h` exists in every module, so an include of
     * `"config.h"` from outside a module is genuinely ambiguous. */
    f = open_file(dir, "config.h");
    if (f == NULL) {
        return -1;
    }
    (void)fprintf(f, "#ifndef ATLAS_FIXTURE_CONFIG_H\n#define ATLAS_FIXTURE_CONFIG_H\n");
    (void)fprintf(f, "#define FIXTURE_CONFIG_ROOT 1\n#endif\n");
    (void)fclose(f);
    return 0;
}

/* One module: a private header, a duplicate `config.h`, and `files` sources. */
static int write_module(const char *root, int m, int files, int is_test, long *lines_out,
                        long *symbols_out) {
    char dir[1024];
    (void)snprintf(dir, sizeof(dir), "%s/%s%d", root, is_test ? "tests/mod" : "src/mod", m);
    if (make_dir(dir) != 0) {
        return -1;
    }

    char name[256];
    FILE *f = open_file(dir, "module.h");
    if (f == NULL) {
        return -1;
    }
    (void)fprintf(f, "#ifndef ATLAS_FIXTURE_MOD%d_H\n#define ATLAS_FIXTURE_MOD%d_H\n", m, m);
    (void)fprintf(f, "#include \"common.h\"\n");
    for (int i = 0; i < files; i++) {
        for (int e = 0; e < ENTRIES_PER_FILE; e++) {
            (void)fprintf(f, "int mod%d_entry_%d_%d(fixture_ctx *ctx, int value);\n", m, i, e);
            *lines_out += 1;
            *symbols_out += 1;
        }
    }
    (void)fprintf(f, "#endif\n");
    *lines_out += 4;
    *symbols_out += 1; /* the guard macro */
    (void)fclose(f);

    /* Every module has its own `config.h`. Including `"config.h"` from a module
     * resolves exactly (same directory); including it from elsewhere is
     * ambiguous across the whole tree, which is the case worth measuring. */
    f = open_file(dir, "config.h");
    if (f == NULL) {
        return -1;
    }
    (void)fprintf(f, "#ifndef ATLAS_FIXTURE_MOD%d_CONFIG_H\n#define ATLAS_FIXTURE_MOD%d_CONFIG_H\n",
                  m, m);
    (void)fprintf(f, "#define FIXTURE_MOD%d_TUNING 4\n#endif\n", m);
    *lines_out += 4;
    *symbols_out += 2;
    (void)fclose(f);

    for (int i = 0; i < files; i++) {
        (void)snprintf(name, sizeof(name), "part_%d.c", i);
        f = open_file(dir, name);
        if (f == NULL) {
            return -1;
        }
        unsigned long long r = mix((unsigned long long)m, (unsigned long long)i);

        (void)fprintf(f, "#include \"module.h\"\n#include \"config.h\"\n");
        /* Roughly a third of the files reach into a neighbouring module, so the
         * dependency graph is a graph rather than a forest. */
        if ((r % 3u) == 0u && m > 0) {
            (void)fprintf(f, "#include \"../mod%d/module.h\"\n", (int)(r % (unsigned)m));
        }
        (void)fprintf(f, "#include <string.h>\n");
        *lines_out += 4;

        /* The same static name in every file. Two of these must never be merged,
         * and a resolver that merged them would make this the most ambiguous
         * symbol in the tree instead of the least. */
        (void)fprintf(f, "static int helper(int v) { return v + %d; }\n", (int)(r % 97u));
        (void)fprintf(f, "static int local_%d_%d(int v) { return helper(v) * 2; }\n", m, i);
        *lines_out += 2;
        *symbols_out += 2;

        (void)fprintf(f, "typedef struct mod%d_state_%d { int a; long b; } mod%d_state_%d;\n", m, i,
                      m, i);
        *lines_out += 1;
        *symbols_out += 2;

        for (int e = 0; e < ENTRIES_PER_FILE; e++) {
            unsigned long long er = mix(r, (unsigned long long)(e + 101));
            (void)fprintf(f, "int mod%d_entry_%d_%d(fixture_ctx *ctx, int value) {\n", m, i, e);
            (void)fprintf(f, "    int acc = local_%d_%d(value);\n", m, i);
            (void)fprintf(f, "    acc += fixture_common_helper(acc);\n");
            (void)fprintf(f, "    acc = FIXTURE_MAX(acc, ctx->depth);\n");
            *lines_out += 4;
            *symbols_out += 1;

            /* Calls into a neighbouring module: resolvable, and the reason the
             * call graph spans modules rather than being a set of islands. */
            for (int k = 0; k < 4; k++) {
                unsigned long long t = mix(er, (unsigned long long)k);
                (void)fprintf(f, "    acc += mod%d_entry_%d_%d(ctx, acc);\n",
                              (int)(t % (unsigned)(m + 1)), (int)((t >> 8) % (unsigned)files),
                              (int)((t >> 16) % ENTRIES_PER_FILE));
                *lines_out += 1;
            }
            /* A call to something nothing defines, so the fixture has a real
             * unresolved population rather than a perfectly closed graph. */
            (void)fprintf(f, "    acc += external_only_%d(acc);\n", (int)(er % 11u));
            /* An indirect call, which must produce no named call candidate. */
            (void)fprintf(f,
                          "    if (ctx->total) { int (*fp)(int) = helper; acc += (*fp)(acc); }\n");
            *lines_out += 2;

            /* A conditional block, so CONDITIONAL is exercised at scale. */
            (void)fprintf(f, "#ifdef FIXTURE_EXTRA_%d\n", (int)(er % 5u));
            (void)fprintf(f, "    acc += fixture_impl_detail(acc);\n");
            (void)fprintf(f, "#endif\n");
            *lines_out += 3;

            (void)fprintf(f, "    /* comment with int fake_decl(void); and \"a string\" */\n");
            (void)fprintf(f, "    return acc;\n}\n");
            *lines_out += 3;
        }
        (void)fclose(f);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        (void)fprintf(stderr, "usage: %s DIR MODULES FILES_PER_MODULE\n", argv[0]);
        return 2;
    }
    const char *root = argv[1];
    int modules = atoi(argv[2]);
    int files = atoi(argv[3]);
    if (modules <= 0 || files <= 0) {
        (void)fprintf(stderr, "atlas-gen-ctree: MODULES and FILES_PER_MODULE must be positive\n");
        return 2;
    }

    char sub[1024];
    if (make_dir(root) != 0) {
        return 1;
    }
    (void)snprintf(sub, sizeof(sub), "%s/src", root);
    if (make_dir(sub) != 0) {
        return 1;
    }
    (void)snprintf(sub, sizeof(sub), "%s/tests", root);
    if (make_dir(sub) != 0) {
        return 1;
    }
    if (write_common(root) != 0) {
        return 1;
    }

    long lines = 0;
    long symbols = 0;
    /* One module in eight is a test module, so the role classifier has a real
     * test subtree to find rather than one directory. */
    for (int m = 0; m < modules; m++) {
        if (write_module(root, m, files, (m % 8) == 7 ? 1 : 0, &lines, &symbols) != 0) {
            return 1;
        }
    }

    /* A compile database covering every source, so build-metadata resolution is
     * exercised at scale too. Written last so a reader of the tree sees it as
     * the generated artefact it is. */
    FILE *f = open_file(root, "compile_commands.json");
    if (f == NULL) {
        return 1;
    }
    (void)fprintf(f, "[\n");
    int first = 1;
    for (int m = 0; m < modules; m++) {
        for (int i = 0; i < files; i++) {
            (void)fprintf(f,
                          "%s  {\"directory\": \"%s\", \"file\": \"%s/mod%d/part_%d.c\", "
                          "\"output\": \"build/mod%d_part_%d.o\", "
                          "\"arguments\": [\"cc\", \"-Iinclude\", \"-I%s/mod%d\", "
                          "\"-DFIXTURE_BUILD=1\", \"-std=c17\", \"-c\", "
                          "\"%s/mod%d/part_%d.c\"]}\n",
                          first ? "" : " ,", root, (m % 8) == 7 ? "tests" : "src", m, i, m, i,
                          (m % 8) == 7 ? "tests" : "src", m, (m % 8) == 7 ? "tests" : "src", m, i);
            first = 0;
        }
    }
    (void)fprintf(f, "]\n");
    (void)fclose(f);

    (void)printf("modules=%d files_per_module=%d source_files=%d approx_lines=%ld "
                 "approx_symbols=%ld\n",
                 modules, files, modules * files, lines, symbols);
    return 0;
}

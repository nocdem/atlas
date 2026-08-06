/* Atlas - the systemd user unit: rendering and installation.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Every case here runs against a temporary XDG_CONFIG_HOME inside the fixture.
 * Nothing is written to the real account, and no service is ever enabled or
 * started — including by the code under test, which is one of the assertions.
 */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/unit.h"
#include "atlas_test.h"
#include "support/fixture.h"

/* Points XDG_CONFIG_HOME inside the fixture for the duration of one test. */
typedef struct scoped_config {
    char *saved;
    bool had;
} scoped_config;

static void config_push(scoped_config *sc, const fixture *fx, atlas_err *err) {
    const char *cur = getenv("XDG_CONFIG_HOME");
    sc->had = (cur != NULL);
    sc->saved = (cur != NULL) ? strdup(cur) : NULL;
    atlas_buf dir = ATLAS_BUF_INIT;
    T_OK(atlas_buf_set(&dir, fx->root.data, fx->root.len, err), err);
    T_OK(atlas_buf_append_str(&dir, "/config", err), err);
    T_REQUIRE(setenv("XDG_CONFIG_HOME", atlas_buf_cstr(&dir), 1) == 0);
    atlas_buf_free(&dir);
}

static void config_pop(scoped_config *sc) {
    if (sc->had && sc->saved != NULL) {
        (void)setenv("XDG_CONFIG_HOME", sc->saved, 1);
    } else {
        (void)unsetenv("XDG_CONFIG_HOME");
    }
    free(sc->saved);
    sc->saved = NULL;
}

static void test_render_contents(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf unit = ATLAS_BUF_INIT;
    T_OK(atlas_unit_render("/usr/local/bin/atlas", NULL, &unit, &err), &err);
    const char *s = atlas_buf_cstr(&unit);

    /* The properties the phase brief requires, asserted individually so a
     * regression names the missing one. */
    T_CHECK_MSG(strstr(s, "ExecStart=/usr/local/bin/atlas daemon run\n") != NULL,
                "ExecStart must be the absolute path with the foreground subcommand");
    T_CHECK_MSG(strstr(s, "Type=simple") != NULL, "the service must be a foreground service");
    T_CHECK_MSG(strstr(s, "Restart=on-failure") != NULL, "Restart=on-failure is required");
    T_CHECK_MSG(strstr(s, "UMask=0077") != NULL, "UMask=0077 is required");
    T_CHECK_MSG(strstr(s, "RuntimeDirectory=atlas") != NULL, "RuntimeDirectory=atlas is required");
    T_CHECK_MSG(strstr(s, "RuntimeDirectoryMode=0700") != NULL,
                "RuntimeDirectoryMode=0700 is required");
    T_CHECK_MSG(strstr(s, "loginctl enable-linger") != NULL,
                "the unit should document the lingering requirement");

    /* Never root, and never a shell. */
    T_CHECK_MSG(strstr(s, "User=root") == NULL, "the unit must not name root");
    T_CHECK_MSG(strstr(s, "sudo") == NULL || strstr(s, "sudo loginctl") != NULL,
                "the only sudo mentioned may be the documented lingering step");
    T_CHECK_MSG(strstr(s, "/bin/sh") == NULL, "the unit must not invoke a shell");
    T_CHECK_MSG(strstr(s, "ExecStart=-") == NULL, "ExecStart must not be prefixed");

    /* No shell interpolation in ExecStart: systemd expands %-specifiers, and one
     * in the executable path would resolve at start time to something else. */
    const char *exec = strstr(s, "ExecStart=");
    T_REQUIRE(exec != NULL);
    const char *eol = strchr(exec, '\n');
    T_REQUIRE(eol != NULL);
    for (const char *p = exec; p < eol; p++) {
        T_CHECK_MSG(*p != '%' && *p != '$' && *p != ';',
                    "ExecStart must contain no specifier or separator characters");
    }
    atlas_buf_free(&unit);
}

static void test_render_with_data_dir(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf unit = ATLAS_BUF_INIT;
    T_OK(atlas_unit_render("/opt/atlas/bin/atlas", "/srv/atlas-data", &unit, &err), &err);
    T_CHECK(strstr(atlas_buf_cstr(&unit),
                   "ExecStart=/opt/atlas/bin/atlas daemon run --data-dir /srv/atlas-data\n") !=
            NULL);
    atlas_buf_free(&unit);
}

static void test_render_refuses_unsafe_paths(void) {
    atlas_err err;
    atlas_buf unit = ATLAS_BUF_INIT;
    static const char *const BAD[] = {
        "relative/atlas",           /* not absolute */
        "/opt/my atlas/atlas",      /* a space would need quoting */
        "/opt/atlas%h/atlas",       /* a systemd specifier */
        "/opt/atlas$X/atlas",       /* a variable reference */
        "/opt/atlas;rm -rf /;/a",   /* a separator */
        "/opt/\"atlas\"/atlas",     /* a quote */
    };
    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        atlas_err_init(&err);
        /* Refused rather than escaped. A unit file is executed, and a subtly
         * mis-escaped ExecStart is command injection with extra steps. */
        T_FAILS_WITH(atlas_unit_render(BAD[i], NULL, &unit, &err), ATLAS_ERR_CONFIG, &err);
    }
    atlas_buf_free(&unit);
}

static void test_install_and_uninstall(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);
    scoped_config sc;
    config_push(&sc, &fx, &err);

    atlas_unit_install_report rep;
    atlas_unit_install_report_init(&rep);
    T_OK(atlas_unit_install("/usr/local/bin/atlas", NULL, false, &rep, &err), &err);
    T_CHECK(rep.wrote_file);
    T_CHECK(!rep.replaced_existing);
    T_CHECK(!rep.unchanged);
    T_CHECK(strstr(atlas_buf_cstr(&rep.path), "/systemd/user/atlas.service") != NULL);

    struct stat sb;
    T_REQUIRE(lstat(atlas_buf_cstr(&rep.path), &sb) == 0);
    T_CHECK_MSG((sb.st_mode & 07777) == 0600, "the unit must be 0600, got %o",
                (unsigned)(sb.st_mode & 07777));
    T_CHECK(S_ISREG(sb.st_mode));

    /* Nothing was enabled: no wants/ symlink was created anywhere. */
    atlas_buf wants = ATLAS_BUF_INIT;
    T_OK(atlas_buf_set(&wants, rep.dir.data, rep.dir.len, &err), &err);
    T_OK(atlas_buf_append_str(&wants, "/default.target.wants/atlas.service", &err), &err);
    T_CHECK_MSG(lstat(atlas_buf_cstr(&wants), &sb) != 0,
                "install must not enable the service");
    atlas_buf_free(&wants);

    /* Installing the same content again is a no-op that says so. */
    atlas_unit_install_report again;
    atlas_unit_install_report_init(&again);
    T_OK(atlas_unit_install("/usr/local/bin/atlas", NULL, false, &again, &err), &err);
    T_CHECK(again.unchanged);
    T_CHECK(!again.wrote_file);
    atlas_unit_install_report_free(&again);

    /* A different executable path replaces the unit Atlas wrote. */
    atlas_unit_install_report moved;
    atlas_unit_install_report_init(&moved);
    T_OK(atlas_unit_install("/opt/atlas/bin/atlas", NULL, false, &moved, &err), &err);
    T_CHECK(moved.replaced_existing);
    T_CHECK(moved.wrote_file);
    atlas_unit_install_report_free(&moved);

    atlas_unit_install_report gone;
    atlas_unit_install_report_init(&gone);
    T_OK(atlas_unit_uninstall(false, &gone, &err), &err);
    T_CHECK(gone.removed);
    T_CHECK(lstat(atlas_buf_cstr(&gone.path), &sb) != 0);

    /* Uninstalling twice is not an error. */
    atlas_unit_install_report twice;
    atlas_unit_install_report_init(&twice);
    T_OK(atlas_unit_uninstall(false, &twice, &err), &err);
    T_CHECK(twice.was_absent);
    T_CHECK(!twice.removed);

    atlas_unit_install_report_free(&twice);
    atlas_unit_install_report_free(&gone);
    atlas_unit_install_report_free(&rep);
    config_pop(&sc);
    fx_close(&fx);
}

static void test_will_not_clobber_a_hand_written_unit(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);
    scoped_config sc;
    config_push(&sc, &fx, &err);

    atlas_buf dir = ATLAS_BUF_INIT;
    atlas_buf path = ATLAS_BUF_INIT;
    T_OK(atlas_unit_dir(&dir, &err), &err);
    T_OK(atlas_unit_path(&path, &err), &err);
    /* Create the directory chain by installing once, then replace the content
     * with something Atlas did not write. */
    atlas_unit_install_report seed;
    atlas_unit_install_report_init(&seed);
    T_OK(atlas_unit_install("/usr/local/bin/atlas", NULL, false, &seed, &err), &err);
    atlas_unit_install_report_free(&seed);

    int fd = open(atlas_buf_cstr(&path), O_WRONLY | O_TRUNC, 0600);
    T_REQUIRE(fd >= 0);
    static const char MINE[] = "[Unit]\nDescription=my careful hand-written unit\n";
    T_REQUIRE(write(fd, MINE, sizeof(MINE) - 1u) == (ssize_t)(sizeof(MINE) - 1u));
    (void)close(fd);

    atlas_unit_install_report rep;
    atlas_unit_install_report_init(&rep);
    atlas_err ierr;
    atlas_err_init(&ierr);
    T_FAILS_WITH(atlas_unit_install("/usr/local/bin/atlas", NULL, false, &rep, &ierr),
                 ATLAS_ERR_INTEGRITY, &ierr);
    T_CHECK(strstr(atlas_err_msg(&ierr), "not written by Atlas") != NULL);

    /* And the file is untouched, not half-replaced. */
    atlas_buf content = ATLAS_BUF_INIT;
    fd = open(atlas_buf_cstr(&path), O_RDONLY);
    T_REQUIRE(fd >= 0);
    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf));
    (void)close(fd);
    T_REQUIRE(n > 0);
    T_OK(atlas_buf_set(&content, buf, (size_t)n, &err), &err);
    T_EQ_STR(atlas_buf_cstr(&content), MINE);

    /* Uninstall refuses it too. */
    atlas_unit_install_report urep;
    atlas_unit_install_report_init(&urep);
    atlas_err uerr;
    atlas_err_init(&uerr);
    T_FAILS_WITH(atlas_unit_uninstall(false, &urep, &uerr), ATLAS_ERR_INTEGRITY, &uerr);

    /* --force is the explicit escape hatch. */
    atlas_unit_install_report forced;
    atlas_unit_install_report_init(&forced);
    T_OK(atlas_unit_install("/usr/local/bin/atlas", NULL, true, &forced, &err), &err);
    T_CHECK(forced.replaced_existing);

    atlas_unit_install_report_free(&forced);
    atlas_unit_install_report_free(&urep);
    atlas_unit_install_report_free(&rep);
    atlas_buf_free(&content);
    atlas_buf_free(&dir);
    atlas_buf_free(&path);
    config_pop(&sc);
    fx_close(&fx);
}

static void test_symlinked_unit_path_refused(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);
    scoped_config sc;
    config_push(&sc, &fx, &err);

    atlas_unit_install_report seed;
    atlas_unit_install_report_init(&seed);
    T_OK(atlas_unit_install("/usr/local/bin/atlas", NULL, false, &seed, &err), &err);
    T_REQUIRE(unlink(atlas_buf_cstr(&seed.path)) == 0);
    /* Writing through this would let anything that can write the unit directory
     * make Atlas overwrite a file elsewhere. */
    T_REQUIRE(symlink("/dev/null", atlas_buf_cstr(&seed.path)) == 0);

    atlas_unit_install_report rep;
    atlas_unit_install_report_init(&rep);
    atlas_err ierr;
    atlas_err_init(&ierr);
    T_FAILS_WITH(atlas_unit_install("/usr/local/bin/atlas", NULL, false, &rep, &ierr),
                 ATLAS_ERR_INTEGRITY, &ierr);
    T_CHECK(strstr(atlas_err_msg(&ierr), "symbolic link") != NULL);
    /* --force does not override this one: it overrides "not ours", not
     * "following this would write somewhere else entirely". */
    atlas_err ferr;
    atlas_err_init(&ferr);
    T_FAILS_WITH(atlas_unit_install("/usr/local/bin/atlas", NULL, true, &rep, &ferr),
                 ATLAS_ERR_INTEGRITY, &ferr);

    atlas_unit_install_report_free(&rep);
    atlas_unit_install_report_free(&seed);
    config_pop(&sc);
    fx_close(&fx);
}

/* The CLI surface: print changes nothing, install needs --user. */
static void test_cli_service_commands(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    atlas_buf out = ATLAS_BUF_INIT;
    atlas_buf errout = ATLAS_BUF_INIT;
    int code = 0;
    const char *print_args[] = {"service", "print"};
    T_OK(fx_atlas(print_args, 2u, &out, &errout, &code, &err), &err);
    T_EQ_INT(code, 0);
    T_CHECK(strstr(atlas_buf_cstr(&out), "[Service]") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&out), "RuntimeDirectoryMode=0700") != NULL);

    /* install without --user must fail, and say why. */
    atlas_buf_reset(&out);
    atlas_buf_reset(&errout);
    const char *bad[] = {"service", "install"};
    T_OK(fx_atlas(bad, 2u, &out, &errout, &code, &err), &err);
    T_EQ_INT(code, (int)ATLAS_ERR_USAGE);
    T_CHECK(strstr(atlas_buf_cstr(&errout), "--user") != NULL);

    atlas_buf_free(&out);
    atlas_buf_free(&errout);
    fx_close(&fx);
}

static const atlas_test TESTS[] = {
    {"the rendered unit has the required properties", test_render_contents},
    {"a data directory override reaches ExecStart", test_render_with_data_dir},
    {"unsafe executable paths are refused, not escaped", test_render_refuses_unsafe_paths},
    {"install and uninstall", test_install_and_uninstall},
    {"a hand-written unit is never clobbered", test_will_not_clobber_a_hand_written_unit},
    {"a symlinked unit path is refused even with --force", test_symlinked_unit_path_refused},
    {"service print changes nothing and install needs --user", test_cli_service_commands},
};

ATLAS_TEST_MAIN("unit", TESTS)

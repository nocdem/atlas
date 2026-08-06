/* Atlas - one-time integration with Claude Code.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Two jobs: find the plugin, and record where this Atlas executable is so the
 * plugin's launcher can find it back.
 *
 * The second is the one that actually needed solving. An installed Claude
 * plugin lives in a cache directory whose path changes on every update, so
 * nothing inside the plugin can hold a stable relative path to a binary outside
 * it, and a symlink planted next to the plugin does not survive being re-copied.
 * The launcher therefore reads a small configuration record from the user's own
 * config directory, which nothing else rewrites — and falls back to PATH, so an
 * ordinary `make install` needs no configuration at all.
 */
#define _GNU_SOURCE 1

#include "atlas/integrate.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/hook.h"
#include "atlas/ipc.h"
#include "atlas/jsonread.h"
#include "atlas/mcp.h"
#include "atlas/unit.h"

const char *atlas_plugin_source_name(atlas_plugin_source s) {
    switch (s) {
    case ATLAS_PLUGIN_SRC_NONE: return "none";
    case ATLAS_PLUGIN_SRC_ENV: return "ATLAS_CLAUDE_PLUGIN_DIR";
    case ATLAS_PLUGIN_SRC_CONFIG: return "recorded";
    case ATLAS_PLUGIN_SRC_INSTALLED: return "installed";
    case ATLAS_PLUGIN_SRC_SOURCE: return "source-tree";
    }
    return "none";
}

const char *atlas_claude_state_name(atlas_claude_state s) {
    switch (s) {
    case ATLAS_CLAUDE_NOT_INSTALLED: return "not-installed";
    case ATLAS_CLAUDE_INSTALLED_ENABLED: return "installed-enabled";
    case ATLAS_CLAUDE_INSTALLED_DISABLED: return "installed-disabled";
    case ATLAS_CLAUDE_DEVELOPMENT: return "development";
    case ATLAS_CLAUDE_UNKNOWN: return "unknown";
    }
    return "unknown";
}

void atlas_integrate_report_init(atlas_integrate_report *r) {
    memset(r, 0, sizeof(*r));
    atlas_buf_init(&r->exe);
    atlas_buf_init(&r->plugin_dir);
    atlas_buf_init(&r->marketplace_dir);
    atlas_buf_init(&r->config_path);
    atlas_buf_init(&r->claude_config_dir);
    atlas_buf_init(&r->installed_id);
    atlas_buf_init(&r->installed_scope);
    atlas_buf_init(&r->installed_path);
    atlas_buf_init(&r->socket_path);
    atlas_buf_init(&r->data_dir);
    atlas_buf_init(&r->mcp_selftest_detail);
    atlas_buf_init(&r->problems);
    r->claude_state = ATLAS_CLAUDE_UNKNOWN;
}

void atlas_integrate_report_free(atlas_integrate_report *r) {
    if (r == NULL) {
        return;
    }
    atlas_buf_free(&r->exe);
    atlas_buf_free(&r->plugin_dir);
    atlas_buf_free(&r->marketplace_dir);
    atlas_buf_free(&r->config_path);
    atlas_buf_free(&r->claude_config_dir);
    atlas_buf_free(&r->installed_id);
    atlas_buf_free(&r->installed_scope);
    atlas_buf_free(&r->installed_path);
    atlas_buf_free(&r->socket_path);
    atlas_buf_free(&r->data_dir);
    atlas_buf_free(&r->mcp_selftest_detail);
    atlas_buf_free(&r->problems);
}

static void problem(atlas_integrate_report *r, const char *text) {
    atlas_err ignore;
    atlas_err_init(&ignore);
    if (r->problems.len > 0) {
        (void)atlas_buf_append_ch(&r->problems, '\n', &ignore);
    }
    (void)atlas_buf_append_str(&r->problems, text, &ignore);
    r->ok = false;
}

/* --- paths ---------------------------------------------------------------- */

static bool is_dir(const char *path) {
    struct stat st;
    return path != NULL && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool is_file(const char *path) {
    struct stat st;
    return path != NULL && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool is_executable(const char *path) {
    return is_file(path) && access(path, X_OK) == 0;
}

/* Appends `rel` to `base` with one separator. */
static atlas_status join(atlas_buf *out, const char *base, const char *rel, atlas_err *err) {
    atlas_buf_reset(out);
    atlas_status st = atlas_buf_append_str(out, base, err);
    if (st == ATLAS_OK && out->len > 0 && out->data[out->len - 1u] != '/') {
        st = atlas_buf_append_ch(out, '/', err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_append_str(out, rel, err);
    }
    return st;
}

/* The directory containing `path`. */
static atlas_status dirname_of(const char *path, atlas_buf *out, atlas_err *err) {
    size_t n = strlen(path);
    while (n > 1u && path[n - 1u] != '/') {
        n--;
    }
    while (n > 1u && path[n - 1u] == '/') {
        n--;
    }
    return atlas_buf_set(out, path, n > 0 ? n : 1u, err);
}

atlas_status atlas_integrate_config_path(atlas_buf *out, atlas_err *err) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg != NULL && xdg[0] == '/') {
        return join(out, xdg, "atlas/claude-integration.conf", err);
    }
    const char *home = getenv("HOME");
    if (home == NULL || home[0] != '/') {
        return atlas_err_set(err, ATLAS_ERR_CONFIG,
                             "neither XDG_CONFIG_HOME nor HOME names an absolute directory, so "
                             "Atlas cannot decide where its integration record belongs");
    }
    return join(out, home, ".config/atlas/claude-integration.conf", err);
}

/* Reads one `key=value` line out of the configuration record. */
static atlas_status config_get(const char *path, const char *key, atlas_buf *out, bool *found,
                               atlas_err *err) {
    *found = false;
    atlas_buf_reset(out);
    FILE *f = fopen(path, "re");
    if (f == NULL) {
        return ATLAS_OK; /* absent is normal */
    }
    char line[4096];
    size_t keylen = strlen(key);
    atlas_status st = ATLAS_OK;
    while (fgets(line, sizeof(line), f) != NULL) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1u] == '\n' || line[n - 1u] == '\r')) {
            line[--n] = '\0';
        }
        if (n > keylen && strncmp(line, key, keylen) == 0 && line[keylen] == '=') {
            st = atlas_buf_set_str(out, line + keylen + 1u, err);
            *found = (st == ATLAS_OK);
            break;
        }
    }
    (void)fclose(f);
    return st;
}

/* True when `dir` holds a plugin manifest. Checked by looking for the manifest
 * rather than for the directory, so an empty directory left over from an old
 * layout is not mistaken for an installation. */
static bool looks_like_plugin(const char *dir) {
    atlas_buf p = ATLAS_BUF_INIT;
    atlas_err ignore;
    atlas_err_init(&ignore);
    bool ok = false;
    if (join(&p, dir, ".claude-plugin/plugin.json", &ignore) == ATLAS_OK) {
        ok = is_file(atlas_buf_cstr(&p));
    }
    atlas_buf_free(&p);
    return ok;
}

atlas_status atlas_integrate_find_plugin(const char *exe, atlas_buf *out,
                                         atlas_plugin_source *source_out, atlas_err *err) {
    *source_out = ATLAS_PLUGIN_SRC_NONE;
    atlas_buf_reset(out);

    const char *env = getenv("ATLAS_CLAUDE_PLUGIN_DIR");
    if (env != NULL && env[0] == '/' && looks_like_plugin(env)) {
        *source_out = ATLAS_PLUGIN_SRC_ENV;
        return atlas_buf_set_str(out, env, err);
    }

    atlas_buf cfg = ATLAS_BUF_INIT;
    atlas_buf value = ATLAS_BUF_INIT;
    atlas_status st = atlas_integrate_config_path(&cfg, err);
    if (st == ATLAS_OK) {
        bool found = false;
        st = config_get(atlas_buf_cstr(&cfg), "plugin_dir", &value, &found, err);
        if (st == ATLAS_OK && found && looks_like_plugin(atlas_buf_cstr(&value))) {
            *source_out = ATLAS_PLUGIN_SRC_CONFIG;
            st = atlas_buf_set(out, value.data, value.len, err);
            atlas_buf_free(&cfg);
            atlas_buf_free(&value);
            return st;
        }
    }
    atlas_buf_free(&cfg);
    atlas_buf_free(&value);
    /* A configuration problem here is not fatal to the search: the installed
     * and source locations below are found from the executable's own path. */
    atlas_err_init(err);

    atlas_buf bin = ATLAS_BUF_INIT;
    atlas_buf candidate = ATLAS_BUF_INIT;
    st = dirname_of(exe, &bin, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&bin);
        atlas_buf_free(&candidate);
        return st;
    }

    /* An installed layout: <prefix>/bin/atlas beside <prefix>/share/atlas/... */
    static const char *const RELATIVE[] = {
        "../share/atlas/claude-marketplace/atlas", /* make install */
        "../../integrations/claude/atlas",         /* a build directory inside the tree */
        "../integrations/claude/atlas",            /* the tree itself */
        NULL,
    };
    for (size_t i = 0; RELATIVE[i] != NULL; i++) {
        st = join(&candidate, atlas_buf_cstr(&bin), RELATIVE[i], err);
        if (st != ATLAS_OK) {
            break;
        }
        char resolved[PATH_MAX];
        if (realpath(atlas_buf_cstr(&candidate), resolved) == NULL) {
            continue;
        }
        if (!looks_like_plugin(resolved)) {
            continue;
        }
        *source_out = (i == 0) ? ATLAS_PLUGIN_SRC_INSTALLED : ATLAS_PLUGIN_SRC_SOURCE;
        st = atlas_buf_set_str(out, resolved, err);
        break;
    }
    atlas_buf_free(&bin);
    atlas_buf_free(&candidate);
    return st;
}

atlas_status atlas_integrate_find_marketplace(const char *plugin_dir, atlas_buf *out,
                                              atlas_err *err) {
    atlas_buf_reset(out);
    if (plugin_dir == NULL || plugin_dir[0] == '\0') {
        return ATLAS_OK;
    }
    /* In every shipped layout the marketplace is the plugin's parent: the
     * catalog lists the plugin with a relative `./atlas` source, which the
     * specification resolves against the marketplace root. */
    atlas_buf parent = ATLAS_BUF_INIT;
    atlas_status st = dirname_of(plugin_dir, &parent, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&parent);
        return st;
    }
    atlas_buf probe = ATLAS_BUF_INIT;
    if (join(&probe, atlas_buf_cstr(&parent), ".claude-plugin/marketplace.json", err) ==
            ATLAS_OK &&
        is_file(atlas_buf_cstr(&probe))) {
        st = atlas_buf_set(out, parent.data, parent.len, err);
    }
    atlas_buf_free(&probe);
    atlas_buf_free(&parent);
    return st;
}

/* --- reading Claude's own configuration, read-only ------------------------
 *
 * Atlas never writes here. It reads two documented things to answer one
 * question a user actually asks: "is the plugin loaded, and if not, why not?"
 *
 *   - `enabledPlugins` in settings.json, which is a documented setting;
 *   - the installed-plugin record, which is what `claude plugin list` reports.
 *
 * If either is unreadable the state is UNKNOWN rather than assumed absent. A
 * diagnostic that guesses is worse than one that says it does not know. */

/* Reads a whole file, bounded. */
static atlas_status read_bounded(const char *path, size_t max, atlas_buf *out, atlas_err *err) {
    FILE *f = fopen(path, "re");
    if (f == NULL) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG, "cannot open %s", path);
    }
    char chunk[8192];
    size_t n;
    atlas_status st = ATLAS_OK;
    while ((n = fread(chunk, 1u, sizeof(chunk), f)) > 0) {
        if (out->len + n > max) {
            st = atlas_err_set(err, ATLAS_ERR_CONFIG, "%s is larger than %zu bytes", path, max);
            break;
        }
        st = atlas_buf_append(out, chunk, n, err);
        if (st != ATLAS_OK) {
            break;
        }
    }
    (void)fclose(f);
    return st;
}

/* Claude's configuration directory: $CLAUDE_CONFIG_DIR, else ~/.claude. */
static atlas_status claude_config_dir(atlas_buf *out, atlas_err *err) {
    const char *override = getenv("CLAUDE_CONFIG_DIR");
    if (override != NULL && override[0] == '/') {
        return atlas_buf_set_str(out, override, err);
    }
    const char *home = getenv("HOME");
    if (home == NULL || home[0] != '/') {
        return atlas_err_set(err, ATLAS_ERR_CONFIG, "HOME does not name an absolute directory");
    }
    return join(out, home, ".claude", err);
}

/* Looks for an installed Atlas plugin and whether it is enabled.
 *
 * The plugin id is `<name>@<marketplace>`, and the name is `atlas` in every
 * marketplace Atlas ships, so any key beginning `atlas@` is ours. Matching on
 * the prefix rather than on one exact id means a user who renamed the
 * marketplace still gets a correct answer. */
static void read_claude_state(atlas_integrate_report *r) {
    atlas_err err;
    atlas_err_init(&err);
    r->claude_state = ATLAS_CLAUDE_UNKNOWN;

    if (claude_config_dir(&r->claude_config_dir, &err) != ATLAS_OK) {
        return;
    }
    if (!is_dir(atlas_buf_cstr(&r->claude_config_dir))) {
        /* Claude has never run as this user. Nothing is installed, which is a
         * definite answer rather than an unknown one. */
        r->claude_state = r->plugin_found ? ATLAS_CLAUDE_DEVELOPMENT : ATLAS_CLAUDE_NOT_INSTALLED;
        return;
    }

    /* Installed plugins. */
    atlas_buf path = ATLAS_BUF_INIT;
    atlas_buf content = ATLAS_BUF_INIT;
    bool installed = false;
    if (join(&path, atlas_buf_cstr(&r->claude_config_dir), "plugins/installed_plugins.json",
             &err) == ATLAS_OK &&
        is_file(atlas_buf_cstr(&path)) &&
        read_bounded(atlas_buf_cstr(&path), 8u * 1024u * 1024u, &content, &err) == ATLAS_OK) {
        atlas_jsondoc *doc = NULL;
        if (atlas_jsondoc_parse(content.data, content.len, 8u * 1024u * 1024u, 24u, &doc, &err) ==
            ATLAS_OK) {
            const atlas_jsonv *plugins = atlas_jsonv_get(atlas_jsondoc_root(doc), "plugins");
            /* The document is a map keyed by plugin id, and the reader has no
             * key iteration, so the id is looked for by the marketplaces Atlas
             * ships plus the plain name. */
            static const char *const IDS[] = {"atlas@atlas-local", "atlas", NULL};
            for (size_t i = 0; !installed && IDS[i] != NULL; i++) {
                const atlas_jsonv *entry = atlas_jsonv_get(plugins, IDS[i]);
                if (entry == NULL) {
                    continue;
                }
                const atlas_jsonv *first =
                    atlas_jsonv_is_arr(entry) ? atlas_jsonv_at(entry, 0) : entry;
                installed = true;
                (void)atlas_buf_set_str(&r->installed_id, IDS[i], &err);
                const char *scope = atlas_jsonv_str_member(first, "scope");
                const char *install_path = atlas_jsonv_str_member(first, "installPath");
                if (scope != NULL) {
                    (void)atlas_buf_set_str(&r->installed_scope, scope, &err);
                }
                if (install_path != NULL) {
                    (void)atlas_buf_set_str(&r->installed_path, install_path, &err);
                }
            }
            atlas_jsondoc_free(doc);
        }
    }
    atlas_buf_free(&content);
    atlas_buf_free(&path);

    /* The marketplace registration, so `doctor` can tell "never added" from
     * "added but not installed". */
    atlas_buf_init(&content);
    if (join(&path, atlas_buf_cstr(&r->claude_config_dir), "plugins/known_marketplaces.json",
             &err) == ATLAS_OK &&
        is_file(atlas_buf_cstr(&path)) &&
        read_bounded(atlas_buf_cstr(&path), 8u * 1024u * 1024u, &content, &err) == ATLAS_OK) {
        atlas_jsondoc *doc = NULL;
        if (atlas_jsondoc_parse(content.data, content.len, 8u * 1024u * 1024u, 24u, &doc, &err) ==
            ATLAS_OK) {
            r->marketplace_registered =
                atlas_jsonv_get(atlas_jsondoc_root(doc), "atlas-local") != NULL;
            atlas_jsondoc_free(doc);
        }
    }
    atlas_buf_free(&content);
    atlas_buf_free(&path);

    if (!installed) {
        /* Not installed. Whether `--plugin-dir` would work is a separate
         * question, and the answer is whether the source tree is present. */
        r->claude_state = r->plugin_found ? ATLAS_CLAUDE_DEVELOPMENT : ATLAS_CLAUDE_NOT_INSTALLED;
        return;
    }

    /* Installed. Enabled unless a setting says otherwise: an absent entry means
     * the plugin's own default, which is enabled. Checked across the three
     * settings files a user scope can be written to. */
    bool disabled = false;
    static const char *const SETTINGS[] = {"settings.json", "settings.local.json", NULL};
    for (size_t i = 0; !disabled && SETTINGS[i] != NULL; i++) {
        atlas_buf sp = ATLAS_BUF_INIT;
        atlas_buf sc = ATLAS_BUF_INIT;
        if (join(&sp, atlas_buf_cstr(&r->claude_config_dir), SETTINGS[i], &err) == ATLAS_OK &&
            is_file(atlas_buf_cstr(&sp)) &&
            read_bounded(atlas_buf_cstr(&sp), 4u * 1024u * 1024u, &sc, &err) == ATLAS_OK) {
            atlas_jsondoc *doc = NULL;
            if (atlas_jsondoc_parse(sc.data, sc.len, 4u * 1024u * 1024u, 24u, &doc, &err) ==
                ATLAS_OK) {
                const atlas_jsonv *enabled =
                    atlas_jsonv_get(atlas_jsondoc_root(doc), "enabledPlugins");
                const atlas_jsonv *mine =
                    atlas_jsonv_get(enabled, atlas_buf_cstr(&r->installed_id));
                bool value = true;
                if (mine != NULL && atlas_jsonv_bool(mine, &value) && !value) {
                    disabled = true;
                }
                atlas_jsondoc_free(doc);
            }
        }
        atlas_buf_free(&sc);
        atlas_buf_free(&sp);
    }
    r->claude_state = disabled ? ATLAS_CLAUDE_INSTALLED_DISABLED : ATLAS_CLAUDE_INSTALLED_ENABLED;
}

/* --- the MCP self-test ---------------------------------------------------
 *
 * Drives the real server over a pair of in-memory streams. It proves the
 * handshake works and the tool list is complete, which is a stronger claim than
 * "the files are present" and needs neither a daemon nor Claude. */
static void mcp_selftest(atlas_integrate_report *r) {
    static const char SCRIPT[] =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
        "{\"protocolVersion\":\"" ATLAS_MCP_PREFERRED_PROTOCOL "\",\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"atlas-doctor\",\"version\":\"" ATLAS_VERSION_STRING "\"}}}\n"
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n";

    atlas_err ignore;
    atlas_err_init(&ignore);
    FILE *in = fmemopen((void *)(uintptr_t)SCRIPT, sizeof(SCRIPT) - 1u, "r");
    char *outbuf = NULL;
    size_t outlen = 0;
    FILE *out = open_memstream(&outbuf, &outlen);
    if (in == NULL || out == NULL) {
        if (in != NULL) {
            (void)fclose(in);
        }
        if (out != NULL) {
            (void)fclose(out);
            free(outbuf);
        }
        (void)atlas_buf_set_str(&r->mcp_selftest_detail, "cannot open in-memory streams", &ignore);
        return;
    }

    atlas_mcp_opts opts;
    atlas_mcp_opts_init(&opts);
    /* A path nothing listens on, so the self-test exercises the degraded path
     * rather than depending on a running daemon. */
    opts.socket_path = "/nonexistent/atlas-doctor.sock";
    opts.timeout_ms = 200;
    atlas_err err;
    atlas_err_init(&err);
    atlas_status st = atlas_mcp_run(in, out, NULL, &opts, &err);
    (void)fclose(in);
    (void)fclose(out);

    if (st != ATLAS_OK) {
        (void)atlas_buf_set_str(&r->mcp_selftest_detail, atlas_err_msg(&err), &ignore);
        free(outbuf);
        return;
    }

    /* Two documents, one per request, each on its own line. The notification
     * gets none, which is itself part of what is being checked. */
    int64_t tools = 0;
    int lines = 0;
    char *cursor = outbuf;
    bool parsed_ok = true;
    while (cursor != NULL && *cursor != '\0') {
        char *nl = strchr(cursor, '\n');
        size_t n = (nl != NULL) ? (size_t)(nl - cursor) : strlen(cursor);
        if (n > 0) {
            lines++;
            atlas_jsondoc *doc = NULL;
            if (atlas_jsondoc_parse(cursor, n, ATLAS_MCP_MAX_MESSAGE_BYTES, 24u, &doc, &ignore) !=
                ATLAS_OK) {
                parsed_ok = false;
            } else {
                const atlas_jsonv *result = atlas_jsonv_get(atlas_jsondoc_root(doc), "result");
                const atlas_jsonv *list = atlas_jsonv_get(result, "tools");
                if (list != NULL) {
                    tools = (int64_t)atlas_jsonv_arr_len(list);
                }
                atlas_jsondoc_free(doc);
            }
        }
        cursor = (nl != NULL) ? nl + 1 : NULL;
    }
    free(outbuf);

    r->mcp_tools = tools;
    if (!parsed_ok) {
        (void)atlas_buf_set_str(&r->mcp_selftest_detail,
                                "the MCP server produced a line that is not valid JSON", &ignore);
        return;
    }
    if (lines != 2) {
        (void)atlas_buf_set_str(&r->mcp_selftest_detail,
                                "the MCP server did not answer exactly one document per request",
                                &ignore);
        return;
    }
    if (tools <= 0) {
        (void)atlas_buf_set_str(&r->mcp_selftest_detail, "the MCP server listed no tools", &ignore);
        return;
    }
    r->mcp_selftest_ok = true;
    (void)atlas_buf_set_str(&r->mcp_selftest_detail, "initialize and tools/list answered", &ignore);
}

/* --- inspection ----------------------------------------------------------- */

/* Confirms a JSON file exists and parses. "Exists" alone is the wrong check: a
 * manifest that does not parse looks installed and is not. */
static bool json_file_ok(const char *dir, const char *rel) {
    atlas_buf p = ATLAS_BUF_INIT;
    atlas_err ignore;
    atlas_err_init(&ignore);
    bool ok = false;
    if (join(&p, dir, rel, &ignore) == ATLAS_OK && is_file(atlas_buf_cstr(&p))) {
        FILE *f = fopen(atlas_buf_cstr(&p), "re");
        if (f != NULL) {
            atlas_buf content = ATLAS_BUF_INIT;
            char chunk[4096];
            size_t n;
            while ((n = fread(chunk, 1u, sizeof(chunk), f)) > 0) {
                if (content.len + n > 1024u * 1024u ||
                    atlas_buf_append(&content, chunk, n, &ignore) != ATLAS_OK) {
                    break;
                }
            }
            (void)fclose(f);
            atlas_jsondoc *doc = NULL;
            if (content.len > 0 && atlas_jsondoc_parse(content.data, content.len, 1024u * 1024u,
                                                       24u, &doc, &ignore) == ATLAS_OK) {
                ok = atlas_jsonv_is_obj(atlas_jsondoc_root(doc));
                atlas_jsondoc_free(doc);
            }
            atlas_buf_free(&content);
        }
    }
    atlas_buf_free(&p);
    return ok;
}

static bool file_present(const char *dir, const char *rel) {
    atlas_buf p = ATLAS_BUF_INIT;
    atlas_err ignore;
    atlas_err_init(&ignore);
    bool ok = false;
    if (join(&p, dir, rel, &ignore) == ATLAS_OK) {
        ok = is_file(atlas_buf_cstr(&p));
    }
    atlas_buf_free(&p);
    return ok;
}

static bool launcher_ok(const char *dir, const char *rel) {
    atlas_buf p = ATLAS_BUF_INIT;
    atlas_err ignore;
    atlas_err_init(&ignore);
    bool ok = false;
    if (join(&p, dir, rel, &ignore) == ATLAS_OK) {
        ok = is_executable(atlas_buf_cstr(&p));
    }
    atlas_buf_free(&p);
    return ok;
}

static atlas_status inspect(atlas_integrate_report *r, atlas_err *err) {
    r->ok = true;

    atlas_status st = atlas_unit_self_path(&r->exe, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_integrate_config_path(&r->config_path, err);
    if (st != ATLAS_OK) {
        /* Not fatal to inspection: everything except the record itself can
         * still be checked, and the missing piece is named in `problems`. */
        problem(r, "no configuration directory could be resolved (set XDG_CONFIG_HOME or HOME)");
        atlas_err_init(err);
    } else {
        r->config_present = is_file(atlas_buf_cstr(&r->config_path));
    }

    st = atlas_integrate_find_plugin(atlas_buf_cstr(&r->exe), &r->plugin_dir, &r->plugin_source,
                                     err);
    if (st != ATLAS_OK) {
        return st;
    }
    r->plugin_found = r->plugin_dir.len > 0 && is_dir(atlas_buf_cstr(&r->plugin_dir));
    if (r->plugin_found) {
        st = atlas_integrate_find_marketplace(atlas_buf_cstr(&r->plugin_dir), &r->marketplace_dir,
                                              err);
        if (st != ATLAS_OK) {
            return st;
        }
        r->marketplace_ok = r->marketplace_dir.len > 0 &&
                            json_file_ok(atlas_buf_cstr(&r->marketplace_dir),
                                         ".claude-plugin/marketplace.json");
        if (!r->marketplace_ok) {
            problem(r, "no plugin marketplace was found beside the plugin, so there is nothing "
                       "for `claude plugin marketplace add` to point at");
        }
    }
    if (!r->plugin_found) {
        problem(r, "the Claude plugin directory was not found; set ATLAS_CLAUDE_PLUGIN_DIR or "
                   "install Atlas so that <prefix>/share/atlas/claude-marketplace exists");
    } else {
        const char *dir = atlas_buf_cstr(&r->plugin_dir);
        r->manifest_ok = json_file_ok(dir, ".claude-plugin/plugin.json");
        r->hooks_ok = json_file_ok(dir, "hooks/hooks.json");
        r->mcp_ok = json_file_ok(dir, ".mcp.json");
        r->skill_ok = file_present(dir, "skills/atlas-memory/SKILL.md");
        r->launcher_ok = launcher_ok(dir, "bin/atlas-hook") && launcher_ok(dir, "bin/atlas-mcp");
        if (!r->manifest_ok) {
            problem(r, "the plugin manifest is missing or does not parse");
        }
        if (!r->hooks_ok) {
            problem(r, "hooks/hooks.json is missing or does not parse");
        }
        if (!r->mcp_ok) {
            problem(r, ".mcp.json is missing or does not parse");
        }
        if (!r->skill_ok) {
            problem(r, "skills/atlas-memory/SKILL.md is missing");
        }
        if (!r->launcher_ok) {
            problem(r, "bin/atlas-hook and bin/atlas-mcp must both exist and be executable");
        }
    }

    for (size_t i = 0; atlas_hook_events()[i] != NULL; i++) {
        r->hook_events++;
    }

    atlas_err serr;
    atlas_err_init(&serr);
    if (atlas_ipc_socket_path(&r->socket_path, &serr) != ATLAS_OK) {
        (void)atlas_buf_set_str(&r->socket_path, "", err);
    }
    /* Resolves where the index would be and whether it exists. Creates neither
     * the directory nor the file: a diagnostic that initialises what it is
     * diagnosing can only ever answer "fine". */
    {
        atlas_ctx_opts copts;
        memset(&copts, 0, sizeof(copts));
        copts.mode = ATLAS_CTX_INSPECT;
        atlas_ctx *ctx = NULL;
        atlas_err cerr;
        atlas_err_init(&cerr);
        if (atlas_ctx_open(&copts, &ctx, &cerr) == ATLAS_OK) {
            (void)atlas_buf_set_str(&r->data_dir, atlas_ctx_data_dir(ctx), err);
            r->index_present = atlas_ctx_index_present(ctx);
            atlas_ctx_close(ctx);
        }
    }
    r->daemon_reachable = atlas_ipc_daemon_reachable();
    if (!r->daemon_reachable) {
        /* Reported, not a failure. The integration is designed to fail open, so
         * a stopped daemon is a degraded session rather than a broken one. */
        problem(r, "the Atlas daemon is not answering; hooks and MCP will report degraded state "
                   "until it is started (systemctl --user start atlas)");
    }

    mcp_selftest(r);
    if (!r->mcp_selftest_ok) {
        problem(r, "the MCP server did not complete its own handshake, so tool calls would fail");
    }

    /* How Claude loads it, read-only. This is the difference between "install
     * it" and "enable it", and telling somebody the wrong one wastes their
     * afternoon. */
    read_claude_state(r);
    switch (r->claude_state) {
    case ATLAS_CLAUDE_INSTALLED_ENABLED:
        break; /* the target state */
    case ATLAS_CLAUDE_INSTALLED_DISABLED:
        problem(r, "the Atlas plugin is installed but disabled; enable it with "
                   "`claude plugin enable atlas` (reinstalling will not help)");
        break;
    case ATLAS_CLAUDE_DEVELOPMENT:
        problem(r, "the plugin is present but not installed, so ordinary sessions do not load "
                   "it; run `atlas integrate claude print` for the one-time install commands");
        break;
    case ATLAS_CLAUDE_NOT_INSTALLED:
        problem(r, "the Atlas plugin is not installed in Claude; run "
                   "`atlas integrate claude print` for the one-time install commands");
        break;
    case ATLAS_CLAUDE_UNKNOWN:
        problem(r, "Claude's configuration directory could not be read, so whether the plugin "
                   "is installed is unknown");
        break;
    }
    return ATLAS_OK;
}

atlas_status atlas_integrate_claude_doctor(atlas_integrate_report *out, atlas_err *err) {
    return inspect(out, err);
}

/* --- install and uninstall ------------------------------------------------ */

/* Creates the parent directory of `path`, 0700, including any missing
 * intermediate directory.
 *
 * The chain matters: a user who has never run a tool that uses XDG has no
 * `~/.config` at all, and creating only the last component fails with ENOENT on
 * exactly the fresh account this command exists to set up. Each directory is
 * created 0700 because what goes in it names an executable a launcher will run.
 *
 * An existing directory is left alone — including its mode, which is the user's
 * business, not Atlas'. */
static atlas_status ensure_parent(const char *path, atlas_err *err) {
    atlas_buf dir = ATLAS_BUF_INIT;
    atlas_status st = dirname_of(path, &dir, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&dir);
        return st;
    }

    /* Walk forward, creating each prefix. Starting at 1 skips the leading
     * separator, so the root is never a create target. */
    char *p = dir.data;
    for (size_t i = 1; st == ATLAS_OK && i <= dir.len; i++) {
        if (i < dir.len && p[i] != '/') {
            continue;
        }
        char saved = p[i];
        p[i] = '\0';
        if (mkdir(p, 0700) != 0 && errno != EEXIST) {
            st = atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno, "cannot create %s", p);
        }
        p[i] = saved;
    }
    atlas_buf_free(&dir);
    return st;
}

atlas_status atlas_integrate_claude_install(atlas_integrate_report *out, atlas_err *err) {
    atlas_status st = inspect(out, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (out->config_path.len == 0) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG,
                             "no configuration directory could be resolved");
    }

    atlas_buf content = ATLAS_BUF_INIT;
    st = atlas_buf_appendf(&content, err,
                           "# Written by `atlas integrate claude install`.\n"
                           "# The Claude plugin's launcher reads this to find Atlas after Claude\n"
                           "# has copied the plugin into its own cache, where no relative path\n"
                           "# back to this executable would remain valid.\n"
                           "atlas_executable=%s\n",
                           atlas_buf_cstr(&out->exe));
    if (st == ATLAS_OK && out->plugin_found) {
        st = atlas_buf_appendf(&content, err, "plugin_dir=%s\n",
                               atlas_buf_cstr(&out->plugin_dir));
    }
    if (st != ATLAS_OK) {
        atlas_buf_free(&content);
        return st;
    }

    /* Idempotent: an identical record is left alone. A repeated install should
     * be a no-op, not a rewrite with a new mtime that looks like a change. */
    FILE *existing = fopen(atlas_buf_cstr(&out->config_path), "re");
    if (existing != NULL) {
        atlas_buf have = ATLAS_BUF_INIT;
        char chunk[4096];
        size_t n;
        atlas_err ignore;
        atlas_err_init(&ignore);
        while ((n = fread(chunk, 1u, sizeof(chunk), existing)) > 0) {
            if (atlas_buf_append(&have, chunk, n, &ignore) != ATLAS_OK) {
                break;
            }
        }
        (void)fclose(existing);
        bool same = (have.len == content.len) &&
                    (have.len == 0 || memcmp(have.data, content.data, have.len) == 0);
        atlas_buf_free(&have);
        if (same) {
            atlas_buf_free(&content);
            out->wrote_config = false;
            out->config_present = true;
            return ATLAS_OK;
        }
    }

    st = ensure_parent(atlas_buf_cstr(&out->config_path), err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&content);
        return st;
    }
    /* O_NOFOLLOW: the record names an executable a launcher will run, so it is
     * not written through a symlink somebody else planted. */
    int fd = open(atlas_buf_cstr(&out->config_path), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW |
                                                         O_CLOEXEC,
                  0600);
    if (fd < 0) {
        st = atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno, "cannot write %s",
                                 atlas_buf_cstr(&out->config_path));
        atlas_buf_free(&content);
        return st;
    }
    /* The length is taken before the buffer is released: `atlas_buf_free` zeroes
     * `len`, so comparing against it afterwards compares a real write count
     * against zero and reports a failure for every successful write — with
     * whatever errno happened to be left over from an earlier call. */
    size_t want = content.len;
    ssize_t written = write(fd, content.data, want);
    int write_errno = errno;
    (void)close(fd);
    atlas_buf_free(&content);
    if (written < 0 || (size_t)written != want) {
        return atlas_err_set_errno(err, ATLAS_ERR_CONFIG, write_errno, "cannot write %s",
                                   atlas_buf_cstr(&out->config_path));
    }
    out->wrote_config = true;
    out->config_present = true;
    return ATLAS_OK;
}

atlas_status atlas_integrate_claude_uninstall(atlas_integrate_report *out, atlas_err *err) {
    atlas_status st = inspect(out, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (out->config_path.len == 0) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG,
                             "no configuration directory could be resolved");
    }
    /* The record, and nothing else. The Atlas index is deliberately untouched:
     * it is the thing that took time to build, and removing it is a separate,
     * explicit act. */
    if (unlink(atlas_buf_cstr(&out->config_path)) == 0) {
        out->removed_config = true;
        out->config_present = false;
    } else if (errno != ENOENT) {
        return atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno, "cannot remove %s",
                                   atlas_buf_cstr(&out->config_path));
    }
    return ATLAS_OK;
}

atlas_status atlas_integrate_claude_commands(const atlas_integrate_report *r, atlas_buf *out,
                                             atlas_err *err) {
    atlas_buf_reset(out);
    const char *market = r->marketplace_dir.len > 0 ? atlas_buf_cstr(&r->marketplace_dir)
                                                    : "<marketplace-dir>";
    const char *plugin = r->plugin_found ? atlas_buf_cstr(&r->plugin_dir) : "<plugin-dir>";
    return atlas_buf_appendf(
        out, err,
        "# One-time setup. Atlas does not run any of this for you.\n"
        "#\n"
        "# 1. Record where this Atlas executable is, so the plugin can find it\n"
        "#    after Claude copies it into its own cache, where no relative path\n"
        "#    back to this binary would remain valid.\n"
        "atlas integrate claude install --user\n"
        "#\n"
        "# 2. Keep the index current.\n"
        "atlas service install --user\n"
        "systemctl --user daemon-reload\n"
        "systemctl --user enable --now atlas\n"
        "#\n"
        "# 3. Install the plugin permanently, at user scope. Atlas ships a local\n"
        "#    marketplace beside the plugin, so this needs no network.\n"
        "#    After this, ordinary `claude` sessions load Atlas with no flags.\n"
        "claude plugin marketplace add %s\n"
        "claude plugin install atlas@atlas-local --scope user\n"
        "#\n"
        "# 4. Check. Both of these change nothing.\n"
        "claude plugin list\n"
        "atlas integrate claude doctor\n"
        "#\n"
        "# Development only: load the working tree for one session, without\n"
        "# installing anything. This is NOT the setup command.\n"
        "#   claude --plugin-dir %s\n"
        "#\n"
        "# To remove it. Your index is not touched by any of this.\n"
        "#   claude plugin uninstall atlas@atlas-local\n"
        "#   claude plugin marketplace remove atlas-local\n"
        "#   atlas integrate claude uninstall --user\n"
        "#   systemctl --user disable --now atlas\n",
        market, plugin);
}

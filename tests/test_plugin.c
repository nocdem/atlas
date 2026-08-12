/* Atlas - the shipped Claude Code plugin, checked without Claude.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * `claude plugin validate` is the authoritative check and it is run during
 * verification, but it needs Claude installed and Claude is not a build
 * dependency. So this suite checks the same files against the published schemas
 * from the other side: every hook the plugin configures is one this binary
 * handles, every launcher exists and is executable, and the manifest holds the
 * fields the specification requires.
 *
 * The property that actually matters here is the one a schema cannot check: the
 * plugin and the binary must agree about which events exist. A plugin
 * configuring an event the binary silently ignores is a plugin that looks
 * installed and does nothing.
 */
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/hook.h"
#include "atlas/jsonread.h"
#include "atlas/mcp.h"
#include "atlas/proc.h"
#include "atlas_test.h"
#include "support/fixture.h"

#ifndef ATLAS_PLUGIN_DIR
#define ATLAS_PLUGIN_DIR "integrations/claude/atlas"
#endif
#ifndef ATLAS_MARKETPLACE_DIR
#define ATLAS_MARKETPLACE_DIR "integrations/claude"
#endif

/* --- reading the shipped files -------------------------------------------- */

static atlas_status read_file(const char *rel, atlas_buf *out, atlas_err *err) {
    atlas_buf path = ATLAS_BUF_INIT;
    atlas_status st = atlas_buf_appendf(&path, err, "%s/%s", ATLAS_PLUGIN_DIR, rel);
    if (st != ATLAS_OK) {
        atlas_buf_free(&path);
        return st;
    }
    FILE *f = fopen(atlas_buf_cstr(&path), "rb");
    if (f == NULL) {
        st = atlas_err_set(err, ATLAS_ERR_CONFIG, "cannot open %s", atlas_buf_cstr(&path));
        atlas_buf_free(&path);
        return st;
    }
    char chunk[8192];
    size_t n;
    while ((n = fread(chunk, 1u, sizeof(chunk), f)) > 0) {
        st = atlas_buf_append(out, chunk, n, err);
        if (st != ATLAS_OK) {
            break;
        }
    }
    (void)fclose(f);
    atlas_buf_free(&path);
    return st;
}

static atlas_jsondoc *read_json(const char *rel, atlas_err *err) {
    atlas_buf content = ATLAS_BUF_INIT;
    T_OK(read_file(rel, &content, err), err);
    atlas_jsondoc *doc = NULL;
    T_OK(atlas_jsondoc_parse(content.data, content.len, 1024u * 1024u, 24u, &doc, err), err);
    atlas_buf_free(&content);
    return doc;
}

static bool plugin_file_is_executable(const char *rel) {
    atlas_buf path = ATLAS_BUF_INIT;
    atlas_err ignore;
    atlas_err_init(&ignore);
    bool ok = false;
    if (atlas_buf_appendf(&path, &ignore, "%s/%s", ATLAS_PLUGIN_DIR, rel) == ATLAS_OK) {
        struct stat st;
        ok = stat(atlas_buf_cstr(&path), &st) == 0 && S_ISREG(st.st_mode) &&
             access(atlas_buf_cstr(&path), X_OK) == 0;
    }
    atlas_buf_free(&path);
    return ok;
}

/* --- tests ---------------------------------------------------------------- */

static void test_manifest_matches_the_specification(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_jsondoc *doc = read_json(".claude-plugin/plugin.json", &err);
    const atlas_jsonv *root = atlas_jsondoc_root(doc);
    T_REQUIRE(atlas_jsonv_is_obj(root));

    /* `name` is the only required field, and it namespaces every component. */
    const char *name = atlas_jsonv_str_member(root, "name");
    T_REQUIRE(name != NULL);
    T_EQ_STR(name, "atlas");
    for (const char *p = name; *p != '\0'; p++) {
        bool kebab = (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-';
        T_CHECK_MSG(kebab, "the plugin name must be kebab-case");
    }
    /* A pinned version, so a user only receives an update when it is bumped
     * rather than on every commit. */
    T_EQ_STR(atlas_jsonv_str_member(root, "version"), ATLAS_VERSION_STRING);
    T_CHECK(atlas_jsonv_str_member(root, "description") != NULL);
    T_CHECK(atlas_jsonv_str_member(root, "license") != NULL);
    T_CHECK(atlas_jsonv_get(root, "keywords") != NULL);
    atlas_jsondoc_free(doc);
}

/* The marketplace is what makes the installation *permanent*.
 *
 * `claude --plugin-dir` loads a plugin for one session and is a development
 * command. An ordinary session loads what `claude plugin install` put in the
 * user's configuration, and that needs a marketplace to install from. Atlas
 * ships one beside the plugin so the whole flow works with no network. */
static void test_marketplace_catalogues_the_plugin(void) {
    atlas_err err;
    atlas_err_init(&err);

    atlas_buf path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&path, &err, "%s/.claude-plugin/marketplace.json",
                           ATLAS_MARKETPLACE_DIR),
         &err);
    atlas_buf content = ATLAS_BUF_INIT;
    FILE *f = fopen(atlas_buf_cstr(&path), "rb");
    T_REQUIRE_MSG(f != NULL, "no marketplace at %s", atlas_buf_cstr(&path));
    char chunk[8192];
    size_t n;
    while ((n = fread(chunk, 1u, sizeof(chunk), f)) > 0) {
        T_OK(atlas_buf_append(&content, chunk, n, &err), &err);
    }
    (void)fclose(f);

    atlas_jsondoc *doc = NULL;
    T_OK(atlas_jsondoc_parse(content.data, content.len, 1024u * 1024u, 24u, &doc, &err), &err);
    const atlas_jsonv *root = atlas_jsondoc_root(doc);
    T_REQUIRE(atlas_jsonv_is_obj(root));

    /* The three required fields. */
    T_EQ_STR(atlas_jsonv_str_member(root, "name"), "atlas-local");
    T_CHECK(atlas_jsonv_str_member2(root, "owner", "name") != NULL);
    const atlas_jsonv *plugins = atlas_jsonv_get(root, "plugins");
    T_REQUIRE(atlas_jsonv_is_arr(plugins));
    T_EQ_INT((long long)atlas_jsonv_arr_len(plugins), 1);

    const atlas_jsonv *entry = atlas_jsonv_at(plugins, 0);
    T_EQ_STR(atlas_jsonv_str_member(entry, "name"), "atlas");
    /* A relative source, resolved against the marketplace root — which is why
     * the catalog and the plugin have to keep their relationship on disk, and
     * why `make install` copies the whole directory rather than just the
     * plugin. */
    const char *source = atlas_jsonv_str_member(entry, "source");
    T_REQUIRE(source != NULL);
    T_EQ_STR(source, "./atlas");
    T_CHECK_MSG(strncmp(source, "./", 2u) == 0, "a relative source must start with ./");
    /* Pinned, so a user receives an update only when the version is bumped. */
    T_EQ_STR(atlas_jsonv_str_member(entry, "version"), ATLAS_VERSION_STRING);
    atlas_jsondoc_free(doc);

    /* And the source actually resolves to the plugin. */
    atlas_buf resolved = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&resolved, &err, "%s/atlas/.claude-plugin/plugin.json",
                           ATLAS_MARKETPLACE_DIR),
         &err);
    struct stat sb;
    T_CHECK_MSG(stat(atlas_buf_cstr(&resolved), &sb) == 0,
                "the marketplace source does not resolve to a plugin manifest");

    atlas_buf_free(&resolved);
    atlas_buf_free(&content);
    atlas_buf_free(&path);
}

static void test_hooks_json_configures_only_events_this_binary_handles(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_jsondoc *doc = read_json("hooks/hooks.json", &err);
    const atlas_jsonv *hooks = atlas_jsonv_get(atlas_jsondoc_root(doc), "hooks");
    T_REQUIRE(atlas_jsonv_is_obj(hooks));

    /* Every event this binary handles is configured, and nothing else is.
     * A plugin configuring an event the binary ignores looks installed and does
     * nothing; a binary handling an event the plugin never sends is dead code. */
    const char *const *known = atlas_hook_events();
    for (size_t i = 0; known[i] != NULL; i++) {
        const atlas_jsonv *entry = atlas_jsonv_get(hooks, known[i]);
        T_CHECK_MSG(entry != NULL, "hooks.json does not configure %s", known[i]);
        if (entry == NULL) {
            continue;
        }
        T_REQUIRE_MSG(atlas_jsonv_is_arr(entry), "%s is not an array", known[i]);
        size_t groups = atlas_jsonv_arr_len(entry);
        T_CHECK_MSG(groups >= 1, "%s has no hook group", known[i]);

        for (size_t g = 0; g < groups; g++) {
            const atlas_jsonv *group = atlas_jsonv_at(entry, g);
            /* Matchers are only legal on events that support them. Inventing one
             * for an event that does not is the kind of mistake that silently
             * stops a hook from firing. */
            const char *matcher = atlas_jsonv_str_member(group, "matcher");
            bool supports_matcher =
                strcmp(known[i], "PreToolUse") == 0 || strcmp(known[i], "PostToolUse") == 0 ||
                strcmp(known[i], "PostToolUseFailure") == 0 ||
                strcmp(known[i], "SessionStart") == 0 || strcmp(known[i], "SessionEnd") == 0 ||
                strcmp(known[i], "PreCompact") == 0 || strcmp(known[i], "PostCompact") == 0 ||
                strcmp(known[i], "SubagentStart") == 0 || strcmp(known[i], "SubagentStop") == 0 ||
                strcmp(known[i], "DirectoryAdded") == 0;
            T_CHECK_MSG(matcher == NULL || supports_matcher,
                        "%s has a matcher but the event does not support one", known[i]);

            const atlas_jsonv *list = atlas_jsonv_get(group, "hooks");
            T_REQUIRE_MSG(atlas_jsonv_is_arr(list), "%s group %zu has no hooks", known[i], g);
            for (size_t k = 0; k < atlas_jsonv_arr_len(list); k++) {
                const atlas_jsonv *hook = atlas_jsonv_at(list, k);
                T_EQ_STR(atlas_jsonv_str_member(hook, "type"), "command");
                const char *command = atlas_jsonv_str_member(hook, "command");
                T_REQUIRE(command != NULL);
                /* ${CLAUDE_PLUGIN_ROOT}, because the installation directory
                 * changes on every plugin update. */
                T_CHECK_MSG(strstr(command, "${CLAUDE_PLUGIN_ROOT}") == command,
                            "%s does not use the plugin root placeholder", known[i]);
                T_CHECK(strstr(command, "/bin/atlas-hook") != NULL);
                /* Exec form: `args` present means no shell tokenises anything,
                 * so a path containing a space cannot become two arguments. */
                const atlas_jsonv *args = atlas_jsonv_get(hook, "args");
                T_REQUIRE_MSG(atlas_jsonv_is_arr(args), "%s does not use exec form", known[i]);
                T_EQ_INT((long long)atlas_jsonv_arr_len(args), 1);
                const char *event_arg = NULL;
                T_CHECK(atlas_jsonv_str(atlas_jsonv_at(args, 0), &event_arg, NULL));
                T_CHECK_MSG(event_arg != NULL && strcmp(event_arg, known[i]) == 0,
                            "%s passes the wrong event argument", known[i]);
                /* A bounded timeout. A hook with no ceiling inherits Claude's
                 * ten-minute default, which is not a thing a person should wait
                 * for while a memory system thinks. */
                int64_t timeout = 0;
                T_CHECK_MSG(atlas_jsonv_int(atlas_jsonv_get(hook, "timeout"), &timeout),
                            "%s has no timeout", known[i]);
                T_CHECK_MSG(timeout > 0 && timeout <= 20, "%s timeout %lld is out of range",
                            known[i], (long long)timeout);
            }
        }
    }

    /* Nothing beyond the known set, and WorktreeCreate in particular: hooking
     * it would replace Claude's own worktree creation. */
    T_CHECK(atlas_jsonv_get(hooks, "WorktreeCreate") == NULL);
    T_CHECK(atlas_jsonv_get(hooks, "PermissionRequest") == NULL);
    T_CHECK(atlas_jsonv_get(hooks, "PermissionDenied") == NULL);
    atlas_jsondoc_free(doc);
}

static void test_mcp_config_points_at_the_launcher(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_jsondoc *doc = read_json(".mcp.json", &err);
    const atlas_jsonv *servers = atlas_jsonv_get(atlas_jsondoc_root(doc), "mcpServers");
    T_REQUIRE(atlas_jsonv_is_obj(servers));
    const atlas_jsonv *memory = atlas_jsonv_get(servers, "memory");
    T_REQUIRE_MSG(memory != NULL, ".mcp.json does not declare the memory server");
    T_EQ_STR(atlas_jsonv_str_member(memory, "type"), "stdio");
    const char *command = atlas_jsonv_str_member(memory, "command");
    T_REQUIRE(command != NULL);
    T_CHECK(strstr(command, "${CLAUDE_PLUGIN_ROOT}") == command);
    T_CHECK(strstr(command, "/bin/atlas-mcp") != NULL);
    atlas_jsondoc_free(doc);
}

static void test_launchers_exist_and_are_executable(void) {
    /* The whole problem the launchers solve is that a plugin cannot hold a
     * stable path to a binary outside itself. If they are not executable after
     * an install, the integration is inert. */
    T_CHECK(plugin_file_is_executable("bin/atlas-hook"));
    T_CHECK(plugin_file_is_executable("bin/atlas-mcp"));

    atlas_err err;
    atlas_err_init(&err);
    atlas_buf resolve = ATLAS_BUF_INIT;
    T_OK(read_file("bin/atlas-resolve.sh", &resolve, &err), &err);
    const char *text = atlas_buf_cstr(&resolve);
    /* No shell eval of a config file: a config file that gets eval'd executes
     * whatever anyone can write into it. */
    T_CHECK(strstr(text, "eval") == NULL);
    T_CHECK(strstr(text, "claude-integration.conf") != NULL);
    T_CHECK(strstr(text, "command -v atlas") != NULL);
    atlas_buf_free(&resolve);

    /* The hook launcher fails open; the MCP launcher does not. Both are
     * deliberate and both are checked, because getting either backwards is
     * invisible until it matters. */
    atlas_buf hook = ATLAS_BUF_INIT;
    T_OK(read_file("bin/atlas-hook", &hook, &err), &err);
    T_CHECK(strstr(atlas_buf_cstr(&hook), "printf '{}\\n'") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&hook), "exit 0") != NULL);
    atlas_buf_free(&hook);

    atlas_buf mcp = ATLAS_BUF_INIT;
    T_OK(read_file("bin/atlas-mcp", &mcp, &err), &err);
    T_CHECK(strstr(atlas_buf_cstr(&mcp), "exit 1") != NULL);
    atlas_buf_free(&mcp);
}

static void test_skill_states_the_contract(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf skill = ATLAS_BUF_INIT;
    T_OK(read_file("skills/atlas-memory/SKILL.md", &skill, &err), &err);
    const char *text = atlas_buf_cstr(&skill);

    /* Frontmatter, with the two fields a skill is discovered by. */
    T_CHECK(strncmp(text, "---\n", 4u) == 0);
    T_CHECK(strstr(text, "\nname: atlas-memory\n") != NULL);
    T_CHECK(strstr(text, "\ndescription: ") != NULL);

    /* The instructions that make the integration automatic rather than
     * something a user has to remember. */
    T_CHECK(strstr(text, "atlas_repo_overview") != NULL);
    T_CHECK(strstr(text, "atlas_file_context") != NULL);
    T_CHECK(strstr(text, "atlas_record_reason") != NULL);
    T_CHECK(strstr(text, "atlas_record_unknown_reason") != NULL);
    /* A4: the structured proposal tool, the read tool, and the sentence that
     * keeps Claude from claiming it approved something. */
    T_CHECK(strstr(text, "atlas_propose_decision") != NULL);
    T_CHECK(strstr(text, "atlas_decisions") != NULL);
    /* The precise contract, not the overclaim it replaced. The skill used to
     * say "You cannot approve anything", which is false for an agent with shell
     * access; what is true is that no Atlas tool approves and that Claude must
     * not run the command itself. */
    T_CHECK(strstr(text, "No Atlas tool approves anything") != NULL);
    T_CHECK(strstr(text, "do not run it yourself") != NULL);
    T_CHECK(strstr(text, "atlas decision approve") != NULL);
    T_CHECK(strstr(text, "is not a signature") != NULL);
    T_CHECK(strstr(text, "UNKNOWN") != NULL);
    T_CHECK(strstr(text, "untrusted") != NULL || strstr(text, "UNTRUSTED") != NULL);
    /* And the rule that keeps it from becoming a manual tool again. */
    T_CHECK(strstr(text, "Never ask the user to run") != NULL);

    /* Short. Every character is paid for in every session that loads it. */
    T_CHECK_MSG(skill.len < 8192u, "the skill is %zu bytes; keep it short", skill.len);
    atlas_buf_free(&skill);
}

static void test_documented_tool_names_are_plugin_scoped_correctly(void) {
    /* Claude exposes a plugin server's tools as
     * mcp__plugin_<plugin>_<server>__<tool>, and that name is generated rather
     * than chosen. Nothing Atlas ships may hardcode it — the skill refers to
     * tools by their bare names, which is what Claude resolves. */
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf skill = ATLAS_BUF_INIT;
    T_OK(read_file("skills/atlas-memory/SKILL.md", &skill, &err), &err);
    T_CHECK(strstr(atlas_buf_cstr(&skill), "mcp__plugin_") == NULL);
    atlas_buf_free(&skill);

    /* And the bare names the skill uses are the ones the server actually
     * exposes. */
    const char *const *names = atlas_mcp_tool_names();
    size_t n = 0;
    for (; names[n] != NULL; n++) {
        /* Nothing here should be scoped or namespaced by Atlas either. */
        T_CHECK(strncmp(names[n], "atlas_", 6u) == 0);
    }
    /* A2 shipped ten; A3 added six structural ones; A4 added four for decision
     * documents; A6 added one, `atlas_gate_check`. Pinned exactly rather than as
     * a floor, so a tool appearing or vanishing is a deliberate change — which
     * is how this caught A4.
     *
     * What A4 and A6 did *not* add is the point. There is no approval tool and
     * no revalidation tool; `tests/test_decision_mcp.c` asserts that no tool
     * name contains an approval or revalidation verb and that no schema
     * declares a capability argument. A6's one addition is a read: it can say
     * that a decision has gone stale and cannot do anything about it.
     *
     * A8-CI added seven, and the same argument applies to all of them: they are
     * `atlas_sem_status`, `atlas_sem_symbol`, `atlas_sem_callers`,
     * `atlas_sem_callees` and `atlas_sem_trace`, and every one is a read. What
     * A8-CI did *not* add is an index tool. Building a semantic index runs a
     * compiler over repository source, so it is an authorised operator action
     * with no MCP surface at all — a model holding every tool in this list
     * still cannot cause a compiler to run.
     *
     * A9.1 added one, `atlas_revise_decision`, and it is the same argument in the
     * other direction: `decision.revise` had existed since A4 and writes a
     * PROPOSED revision by a MODEL_PROPOSAL actor — exactly what
     * `atlas_propose_decision` writes — so MCP being unable to express it was an
     * accidental gap rather than a boundary. What stays absent is every lifecycle
     * verb: approve, reject, supersede, revalidate and resolve have no tool here,
     * live in the operator-uid RPC group, and need a capability only the terminal
     * channel can obtain. */
    T_CHECK_MSG(n == 29, "expected 29 tools, found %zu", n);
}

/* --- the integration record ----------------------------------------------
 *
 * The one file `atlas integrate claude install --user` writes, and the reason it
 * exists: an installed plugin lives in a cache directory whose path changes on
 * every update, so nothing inside it can hold a stable path to a binary outside
 * it. This drives the whole cycle against a temporary HOME.
 *
 * Both of the bugs this test was written for were real: a fresh account has no
 * `~/.config` at all, so creating only the last directory failed with ENOENT on
 * exactly the account the command exists to set up; and the write length was
 * compared against a buffer that had already been freed, so every successful
 * write reported a failure. */

static void test_install_writes_one_record_and_uninstall_removes_it(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_OK(fx_open(&fx, &err), &err);

    /* A HOME with nothing in it, not even `.config`. */
    atlas_buf home_env = ATLAS_BUF_INIT;
    atlas_buf plugin_env = ATLAS_BUF_INIT;
    atlas_buf conf_path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&home_env, &err, "HOME=%s/home", fx.root.data), &err);
    T_OK(atlas_buf_appendf(&plugin_env, &err, "ATLAS_CLAUDE_PLUGIN_DIR=%s", ATLAS_PLUGIN_DIR),
         &err);
    T_OK(atlas_buf_appendf(&conf_path, &err, "%s/home/.config/atlas/claude-integration.conf",
                           fx.root.data),
         &err);
    T_OK(fx_mkdir(fx.root.data, "home", &err), &err);

    const char *env[] = {atlas_buf_cstr(&home_env), atlas_buf_cstr(&plugin_env), NULL};
    const char *install[] = {"integrate", "claude", "install", "--user", "--json"};
    atlas_buf out = ATLAS_BUF_INIT;
    int code = 0;
    T_OK(fx_atlas_env(install, 5u, env, &out, NULL, &code, &err), &err);
    T_CHECK_MSG(code == 0, "install exited %d: %s", code, atlas_buf_cstr(&out));

    atlas_jsondoc *doc = NULL;
    T_OK(atlas_jsondoc_parse(out.data, out.len, 1024u * 1024u, 24u, &doc, &err), &err);
    const atlas_jsonv *root = atlas_jsondoc_root(doc);
    bool wrote = false;
    T_CHECK(atlas_jsonv_bool(atlas_jsonv_get(root, "wrote_config"), &wrote));
    T_CHECK_MSG(wrote, "install did not write the record on a fresh HOME");
    /* Stated as fields so a caller can check that installing did nothing else. */
    bool flag = true;
    T_CHECK(atlas_jsonv_bool(atlas_jsonv_get(root, "claude_configured"), &flag) && !flag);
    T_CHECK(atlas_jsonv_bool(atlas_jsonv_get(root, "service_enabled"), &flag) && !flag);
    T_CHECK(atlas_jsonv_bool(atlas_jsonv_get(root, "removed_index"), &flag) && !flag);
    atlas_jsondoc_free(doc);

    /* The record names this executable, so the launcher can find it back. */
    atlas_buf record = ATLAS_BUF_INIT;
    FILE *f = fopen(atlas_buf_cstr(&conf_path), "rb");
    T_REQUIRE_MSG(f != NULL, "the integration record was not created at %s",
                  atlas_buf_cstr(&conf_path));
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1u, sizeof(chunk), f)) > 0) {
        T_OK(atlas_buf_append(&record, chunk, n, &err), &err);
    }
    (void)fclose(f);
    T_CHECK(strstr(atlas_buf_cstr(&record), "atlas_executable=") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&record), ATLAS_BIN) != NULL);

    /* The launcher resolves through it, with no atlas on PATH. */
    atlas_buf launcher = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&launcher, &err, "%s/bin/atlas-hook", ATLAS_PLUGIN_DIR), &err);
    atlas_buf home_only = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&home_only, &err, "HOME=%s/home", fx.root.data), &err);
    const char *largv[] = {atlas_buf_cstr(&launcher), "SessionStart", NULL};
    /* A PATH with the ordinary utilities on it — the launcher needs `dirname`
     * and `grep` — but the record is consulted *before* PATH, so this proves the
     * record resolved it whether or not an Atlas happens to be installed
     * system-wide on the machine running the suite. */
    const char *lenv[] = {"PATH=/usr/bin:/bin", atlas_buf_cstr(&home_only),
                          "XDG_RUNTIME_DIR=/nonexistent-atlas-runtime", NULL};
    atlas_proc_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.argv = largv;
    opts.env = lenv;
    opts.timeout_ms = 30000;
    atlas_buf lout = ATLAS_BUF_INIT;
    atlas_proc_result res;
    T_OK(atlas_proc_run(&opts, atlas_proc_sink_buf, &lout, NULL, &res, &err), &err);
    T_EQ_INT(res.exit_code, 0);
    /* `{}` and not the "no atlas executable found" fallback: the record is what
     * made the difference, since PATH holds nothing. */
    T_CHECK_MSG(strstr(atlas_buf_cstr(&lout), "{}") != NULL,
                "the launcher did not resolve atlas through the record: %s",
                atlas_buf_cstr(&lout));

    /* Re-installing is a no-op rather than a rewrite. */
    atlas_buf_reset(&out);
    T_OK(fx_atlas_env(install, 5u, env, &out, NULL, &code, &err), &err);
    T_OK(atlas_jsondoc_parse(out.data, out.len, 1024u * 1024u, 24u, &doc, &err), &err);
    T_CHECK(atlas_jsonv_bool(atlas_jsonv_get(atlas_jsondoc_root(doc), "wrote_config"), &wrote));
    T_CHECK_MSG(!wrote, "a repeated install rewrote the record");
    atlas_jsondoc_free(doc);

    /* Uninstall removes that file and says plainly that it removed nothing
     * else. */
    const char *uninstall[] = {"integrate", "claude", "uninstall", "--user", "--json"};
    atlas_buf_reset(&out);
    T_OK(fx_atlas_env(uninstall, 5u, env, &out, NULL, &code, &err), &err);
    T_OK(atlas_jsondoc_parse(out.data, out.len, 1024u * 1024u, 24u, &doc, &err), &err);
    T_CHECK(atlas_jsonv_bool(atlas_jsonv_get(atlas_jsondoc_root(doc), "removed_config"), &flag) &&
            flag);
    T_CHECK(atlas_jsonv_bool(atlas_jsonv_get(atlas_jsondoc_root(doc), "removed_index"), &flag) &&
            !flag);
    atlas_jsondoc_free(doc);
    T_CHECK_MSG(fopen(atlas_buf_cstr(&conf_path), "rb") == NULL,
                "uninstall left the record behind");

    /* --user is required rather than assumed, so a command copied from
     * documentation that expected otherwise fails loudly. */
    const char *no_user[] = {"integrate", "claude", "install"};
    T_OK(fx_atlas_env(no_user, 3u, env, NULL, NULL, &code, &err), &err);
    T_EQ_INT(code, 2);

    atlas_buf_free(&out);
    atlas_buf_free(&lout);
    atlas_buf_free(&launcher);
    atlas_buf_free(&home_only);
    atlas_buf_free(&record);
    atlas_buf_free(&conf_path);
    atlas_buf_free(&home_env);
    atlas_buf_free(&plugin_env);
    fx_close(&fx);
}

/* --- diagnostics must not initialise what they diagnose -------------------
 *
 * `make doctor` used to create `~/.local/share/atlas` and an empty index as a
 * side effect of asking whether one was healthy. That is wrong twice: the
 * answer is then always "fine", and the question cannot be asked at all on a
 * machine where Atlas has never run — which is exactly when somebody asks it.
 *
 * `make doctor` is literally `atlas doctor` with no arguments, so testing the
 * command tests the target. */

/* Records every path under `dir`, sorted, into `out` one per line. */
static atlas_status snapshot(const char *dir, atlas_buf *out, atlas_err *err);

static atlas_status snapshot_into(const char *dir, atlas_buf *out, atlas_err *err) {
    DIR *d = opendir(dir);
    if (d == NULL) {
        return ATLAS_OK; /* absent is a valid snapshot */
    }
    atlas_status st = ATLAS_OK;
    struct dirent *e;
    while (st == ATLAS_OK && (e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        atlas_buf child = ATLAS_BUF_INIT;
        st = atlas_buf_appendf(&child, err, "%s/%s", dir, e->d_name);
        if (st == ATLAS_OK) {
            st = atlas_buf_appendf(out, err, "%s\n", atlas_buf_cstr(&child));
        }
        if (st == ATLAS_OK) {
            struct stat sb;
            if (stat(atlas_buf_cstr(&child), &sb) == 0 && S_ISDIR(sb.st_mode)) {
                st = snapshot_into(atlas_buf_cstr(&child), out, err);
            }
        }
        atlas_buf_free(&child);
    }
    (void)closedir(d);
    return st;
}

static atlas_status snapshot(const char *dir, atlas_buf *out, atlas_err *err) {
    atlas_buf_reset(out);
    return snapshot_into(dir, out, err);
}

/* Compares two snapshots as sets, ignoring order, and reports the first path
 * that appeared. Sorting would need a comparator; a quadratic scan over a few
 * dozen entries is cheaper to read and fast enough. */
static bool snapshot_grew(const atlas_buf *before, const atlas_buf *after, atlas_buf *added) {
    atlas_err ignore;
    atlas_err_init(&ignore);
    atlas_buf_reset(added);
    const char *p = atlas_buf_cstr(after);
    while (*p != '\0') {
        const char *nl = strchr(p, '\n');
        size_t len = (nl != NULL) ? (size_t)(nl - p) : strlen(p);
        if (len > 0) {
            bool seen = false;
            const char *q = atlas_buf_cstr(before);
            while (!seen && *q != '\0') {
                const char *qn = strchr(q, '\n');
                size_t qlen = (qn != NULL) ? (size_t)(qn - q) : strlen(q);
                seen = (qlen == len && memcmp(q, p, len) == 0);
                if (qn == NULL) {
                    break;
                }
                q = qn + 1;
            }
            if (!seen) {
                (void)atlas_buf_append(added, p, len, &ignore);
                return true;
            }
        }
        if (nl == NULL) {
            break;
        }
        p = nl + 1;
    }
    return false;
}

static void test_diagnostics_create_nothing(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_OK(fx_open(&fx, &err), &err);

    /* A HOME with nothing in it. Every XDG variable points inside it, so a
     * command that creates anything creates it here and is caught. */
    T_OK(fx_mkdir(fx.root.data, "home", &err), &err);
    atlas_buf home = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&home, &err, "%s/home", fx.root.data), &err);

    atlas_buf e_home = ATLAS_BUF_INIT;
    atlas_buf e_data = ATLAS_BUF_INIT;
    atlas_buf e_config = ATLAS_BUF_INIT;
    atlas_buf e_runtime = ATLAS_BUF_INIT;
    atlas_buf e_claude = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&e_home, &err, "HOME=%s", atlas_buf_cstr(&home)), &err);
    T_OK(atlas_buf_appendf(&e_data, &err, "XDG_DATA_HOME=%s/.local/share", atlas_buf_cstr(&home)),
         &err);
    T_OK(atlas_buf_appendf(&e_config, &err, "XDG_CONFIG_HOME=%s/.config", atlas_buf_cstr(&home)),
         &err);
    T_OK(atlas_buf_appendf(&e_runtime, &err, "XDG_RUNTIME_DIR=%s/run", atlas_buf_cstr(&home)),
         &err);
    T_OK(atlas_buf_appendf(&e_claude, &err, "CLAUDE_CONFIG_DIR=%s/.claude", atlas_buf_cstr(&home)),
         &err);
    const char *env[] = {atlas_buf_cstr(&e_home),    atlas_buf_cstr(&e_data),
                         atlas_buf_cstr(&e_config),  atlas_buf_cstr(&e_runtime),
                         atlas_buf_cstr(&e_claude),  NULL};

    atlas_buf before = ATLAS_BUF_INIT;
    atlas_buf after = ATLAS_BUF_INIT;
    atlas_buf added = ATLAS_BUF_INIT;
    T_OK(snapshot(atlas_buf_cstr(&home), &before, &err), &err);

    /* Both diagnostics, in both output modes. */
    static const char *const RUNS[][3] = {
        {"doctor", NULL, NULL},
        {"doctor", "--json", NULL},
        {"integrate", "claude", "doctor"},
    };
    static const size_t NARGS[] = {1u, 2u, 3u};
    for (size_t i = 0; i < sizeof(NARGS) / sizeof(NARGS[0]); i++) {
        atlas_buf out = ATLAS_BUF_INIT;
        int code = 0;
        T_OK(fx_atlas_env(RUNS[i], NARGS[i], env, &out, NULL, &code, &err), &err);
        /* `doctor` on a machine with no index is a correct, healthy answer: the
         * absence is a finding, not a fault. */
        if (NARGS[i] < 3u) {
            T_CHECK_MSG(code == 0, "atlas doctor exited %d on a fresh HOME: %s", code,
                        atlas_buf_cstr(&out));
        }
        T_OK(snapshot(atlas_buf_cstr(&home), &after, &err), &err);
        T_CHECK_MSG(!snapshot_grew(&before, &after, &added),
                    "run %zu created \"%s\"", i, atlas_buf_cstr(&added));
        atlas_buf_free(&out);
    }

    /* Named explicitly, because these are the six things that used to appear. */
    static const char *const MUST_NOT_EXIST[] = {
        ".local/share/atlas",          ".local/share/atlas/atlas.db",
        ".local/share/atlas/atlas.lock", ".config/atlas",
        ".claude",                     "run/atlas",
        NULL,
    };
    for (size_t i = 0; MUST_NOT_EXIST[i] != NULL; i++) {
        atlas_buf p = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&p, &err, "%s/%s", atlas_buf_cstr(&home), MUST_NOT_EXIST[i]), &err);
        struct stat sb;
        T_CHECK_MSG(stat(atlas_buf_cstr(&p), &sb) != 0, "a diagnostic created %s",
                    MUST_NOT_EXIST[i]);
        atlas_buf_free(&p);
    }

    /* And it reported the absence rather than hiding it. */
    atlas_buf out = ATLAS_BUF_INIT;
    int code = 0;
    const char *json[] = {"doctor", "--json"};
    T_OK(fx_atlas_env(json, 2u, env, &out, NULL, &code, &err), &err);
    T_CHECK(strstr(atlas_buf_cstr(&out), "\"index_present\":false") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&out), "\"data_dir_present\":false") != NULL);
    atlas_buf_free(&out);

    atlas_buf_free(&added);
    atlas_buf_free(&after);
    atlas_buf_free(&before);
    atlas_buf_free(&e_claude);
    atlas_buf_free(&e_runtime);
    atlas_buf_free(&e_config);
    atlas_buf_free(&e_data);
    atlas_buf_free(&e_home);
    atlas_buf_free(&home);
    fx_close(&fx);
}

static const atlas_test TESTS[] = {
    {"the manifest matches the plugin specification", test_manifest_matches_the_specification},
    {"the marketplace catalogues the plugin for permanent installation",
     test_marketplace_catalogues_the_plugin},
    {"hooks.json configures exactly the events this binary handles",
     test_hooks_json_configures_only_events_this_binary_handles},
    {".mcp.json points at the launcher", test_mcp_config_points_at_the_launcher},
    {"the launchers exist, are executable, and fail the right way",
     test_launchers_exist_and_are_executable},
    {"the skill states the automatic-use contract", test_skill_states_the_contract},
    {"tool names are bare rather than plugin-scoped",
     test_documented_tool_names_are_plugin_scoped_correctly},
    {"doctor and integrate doctor create nothing at all",
     test_diagnostics_create_nothing},
    {"install writes one record on a fresh HOME and uninstall removes only it",
     test_install_writes_one_record_and_uninstall_removes_it},
};

ATLAS_TEST_MAIN("plugin", TESTS)

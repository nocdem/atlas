/* Atlas - the published MCP argument keys are enforced on every tool.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas_test.h"
#include "mcp/mcp_internal.h"

/* Exercise the real JSON-RPC dispatcher, including remote-only tools. Scopes
 * are injected to isolate validation from authentication (covered by gateway
 * tests). An explicit nonexistent socket prevents every call reaching a DB. */
static atlas_jsondoc *exchange(const char *request, bool remote, atlas_err *err) {
    char *bytes = NULL;
    size_t len = 0;
    FILE *mem = open_memstream(&bytes, &len);
    T_REQUIRE(mem != NULL);
    atlas_mcp_opts opts;
    atlas_mcp_opts_init(&opts);
    opts.socket_path = "/nonexistent-atlas-runtime/atlas.sock";
    atlas_mcp_server server;
    atlas_mcp_server_init(&server, NULL, mem, NULL, &opts);
    server.remote = remote;
    server.granted = UINT32_MAX;
    server.got_initialize = true;
    server.initialized = true;
    T_OK(atlas_mcp_handle_document(&server, request, strlen(request), err), err);
    atlas_mcp_server_teardown(&server);
    T_REQUIRE(fclose(mem) == 0);
    atlas_jsondoc *doc = NULL;
    T_OK(atlas_jsondoc_parse(bytes, len, ATLAS_MCP_MAX_RESULT_BYTES,
                              ATLAS_IPC_MAX_JSON_DEPTH, &doc, err), err);
    free(bytes);
    return doc;
}

static void test_every_tool_refuses_extra_keys(void) {
    atlas_err err;
    atlas_err_init(&err);
    const char *const *names = atlas_mcp_tool_names();
    static const char *const BAD_ARGS[] = {
        "{\"unexpected_argument\":true}",
        "{\"repo\\u0000hidden\":\"proj\"}"
    };
    for (size_t i = 0; names[i] != NULL; i++) {
        for (size_t k = 0; k < sizeof BAD_ARGS / sizeof BAD_ARGS[0]; k++) {
            for (int remote = 0; remote <= 1; remote++) {
                if (!remote && atlas_mcp_tool_remote_only(names[i])) {
                    continue;
                }
                atlas_buf request = ATLAS_BUF_INIT;
                T_OK(atlas_buf_appendf(&request, &err,
                        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                        "\"params\":{\"name\":\"%s\",\"arguments\":%s}}",
                        names[i], BAD_ARGS[k]), &err);
                atlas_jsondoc *doc = exchange(request.data, remote != 0, &err);
                const atlas_jsonv *error = atlas_jsonv_get(atlas_jsondoc_root(doc), "error");
                int64_t code = 0;
                T_CHECK_MSG(atlas_jsonv_int(atlas_jsonv_get(error, "code"), &code) &&
                            code == ATLAS_MCP_INVALID_PARAMS,
                            "%s accepted extra keys on transport %d", names[i], remote);
                const char *message = atlas_jsonv_str_member(error, "message");
                T_CHECK_MSG(message != NULL && strstr(message,
                            k == 0 ? "not a recognised argument" : "embedded NUL") != NULL,
                            "%s did not refuse the offending key", names[i]);
                atlas_jsondoc_free(doc);
                atlas_buf_free(&request);
            }
        }
    }
}

static void test_every_published_key_reaches_typed_validation(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_jsondoc *listing = exchange(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}", true, &err);
    const atlas_jsonv *tools = atlas_jsonv_get(
        atlas_jsonv_get(atlas_jsondoc_root(listing), "result"), "tools");
    size_t count = atlas_jsonv_arr_len(tools);
    size_t expected = 0;
    const char *const *names = atlas_mcp_tool_names();
    while (names[expected] != NULL) {
        expected++;
    }
    T_EQ_INT((int64_t)count, (int64_t)expected);
    for (size_t i = 0; i < count; i++) {
        const atlas_jsonv *tool = atlas_jsonv_at(tools, i);
        const char *name = atlas_jsonv_str_member(tool, "name");
        const atlas_jsonv *properties = atlas_jsonv_get(
            atlas_jsonv_get(tool, "inputSchema"), "properties");
        char *bytes = NULL;
        size_t len = 0;
        FILE *mem = open_memstream(&bytes, &len);
        T_REQUIRE(mem != NULL);
        /* Using declarations as values supplies every published key. Values
         * deliberately fail typed validation; the key check must accept them.
         * No valid operation can reach a daemon at the nonexistent socket. */
        atlas_json *j = atlas_json_new(mem, &err);
        T_REQUIRE(j != NULL);
        T_OK(atlas_jsonv_write(properties, j, ATLAS_IPC_MAX_JSON_DEPTH, &err), &err);
        T_OK(atlas_json_finish(j, &err), &err);
        T_REQUIRE(fclose(mem) == 0);
        atlas_buf request = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&request, &err,
                "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                "\"params\":{\"name\":\"%s\",\"arguments\":%s}}", name, bytes), &err);
        atlas_jsondoc *doc = exchange(request.data, true, &err);
        T_CHECK_MSG(atlas_jsonv_get(atlas_jsondoc_root(doc), "error") == NULL,
                    "%s refused one of its own published keys", name);
        T_CHECK(atlas_jsonv_get(atlas_jsondoc_root(doc), "result") != NULL);
        atlas_jsondoc_free(doc);
        atlas_buf_free(&request);
        free(bytes);
    }
    atlas_jsondoc_free(listing);
}

static const atlas_test TESTS[] = {
    {"every tool rejects extra and NUL-bearing keys on each transport",
     test_every_tool_refuses_extra_keys},
    {"all published keys pass dispatch and reach typed validation",
     test_every_published_key_reaches_typed_validation},
};

ATLAS_TEST_MAIN("mcp_arguments", TESTS)

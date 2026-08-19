/* Atlas - A11.1: the gate wire form, from the sender's encoder to the daemon's
 * decoder.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A gate declared on the command line is not run by the process that parsed it.
 * `src/core/service_orch.c` encodes each one as a length-prefixed argv vector,
 * the daemon decodes it in `src/ipc/server_orch.c`, and the bytes it decodes are
 * the bytes the database stores. Three spellings of one format, and until this
 * suite existed nothing compared them: every A11 test builds its operation in
 * process and hands `atlas_orch_apply` a vector it constructed itself, so the
 * whole encode/transmit/decode path was unexercised.
 *
 * It was also wrong. The daemon wrapped each element in a further `1:` before
 * decoding, and the sender had already written that count, so the decoder read
 * the sender's count as an *argument* count and swallowed the rest of the
 * encoding as one argument. `cmake --build build ...` arrived as the
 * one-element vector `["5:cmake"]`. Nothing downstream could recover from that
 * — `5:cmake` is not on the validation allowlist — so no run submitted through
 * the CLI could ever pass a gate, and therefore no run could ever be accepted.
 *
 * The case that matters is a gate with *more than one word*. A single-word gate
 * survives the defect almost intact, which is part of why it lasted.
 */
#include <string.h>

#include "atlas/orch.h"
#include "atlas/validate.h"
#include "atlas_test.h"

/* The real gate this defect was found with, word for word. */
static const char *const GATE[] = {"cmake",  "--build", "build", "--target",
                                   "test_a11_head_drift", "-j", "4"};
static const size_t GATE_N = sizeof GATE / sizeof GATE[0];

static void build_gate(atlas_orch_argv *a, atlas_err *err) {
    atlas_orch_argv_init(a);
    for (size_t i = 0; i < GATE_N; i++) {
        T_OK(atlas_orch_argv_push(a, GATE[i], strlen(GATE[i]), err), err);
    }
}

/* --- 1: what the sender writes is what the daemon reads -------------------- */

static void test_a_multi_word_gate_survives_the_wire(void) {
    atlas_err err;
    atlas_err_init(&err);

    atlas_orch_argv one;
    build_gate(&one, &err);

    /* Exactly what `src/core/service_orch.c` puts on the wire for one gate. */
    atlas_buf enc = ATLAS_BUF_INIT;
    T_OK(atlas_orch_validations_encode(&one, 1u, &enc, &err), &err);

    /* The canonical form is pinned here rather than described, because the
     * defect was a disagreement about it and prose cannot fail a build. One
     * command, seven arguments, each a counted string. */
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&enc),
                       "1:7:5:cmake,7:--build,5:build,8:--target,19:test_a11_head_drift,2:-j,1:4,")
                    == 0,
                "the gate wire form changed");

    /* The function `src/ipc/server_orch.c` calls — the same one, not a
     * reconstruction of it, so re-inlining a different decode there fails
     * here rather than in a pilot. */
    atlas_orch_argv got[1];
    atlas_orch_argv_init(&got[0]);
    T_OK(atlas_orch_validation_wire_decode(atlas_buf_cstr(&enc), &got[0], &err), &err);

    T_EQ_INT((int)got[0].count, (int)GATE_N);
    for (size_t i = 0; i < GATE_N && i < got[0].count; i++) {
        T_CHECK_MSG(strcmp(atlas_buf_cstr(&got[0].args[i]), GATE[i]) == 0,
                    "argument %zu did not survive the wire", i);
    }

    atlas_orch_argv_free(&got[0]);
    atlas_buf_free(&enc);
    atlas_orch_argv_free(&one);
}

/* --- 2: the wrapper that caused it is named, so it cannot come back quietly - */

static void test_wrapping_the_element_again_destroys_it(void) {
    atlas_err err;
    atlas_err_init(&err);

    atlas_orch_argv one;
    build_gate(&one, &err);
    atlas_buf enc = ATLAS_BUF_INIT;
    T_OK(atlas_orch_validations_encode(&one, 1u, &enc, &err), &err);

    /* The removed line, reproduced: `atlas_buf_appendf(&one, "1:%s", enc)`.
     * This is not how the daemon reads a gate any more, and the assertions
     * below are what "any more" means. Keeping the broken composition in a test
     * is deliberate — the defect was invisible precisely because no test ever
     * decoded a real encoded element, so the repair is only durable if
     * something fails when the wrapper returns. */
    atlas_buf wrapped = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&wrapped, &err, "1:%s", atlas_buf_cstr(&enc)), &err);

    atlas_orch_argv got[1];
    atlas_orch_argv_init(&got[0]);
    atlas_status st = atlas_orch_validation_wire_decode(atlas_buf_cstr(&wrapped), &got[0], &err);

    /* It does not fail. That is the whole problem: it decodes cleanly into one
     * argument that is a fragment of the encoding, and a fragment is not a
     * program. Had it errored, the defect would have surfaced the first time an
     * operator declared a gate. */
    T_OK(st, &err);
    T_EQ_INT((int)got[0].count, 1);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&got[0].args[0]), "5:cmake") == 0,
                "the double-wrapped form decoded to something else again");
    T_CHECK_MSG(!atlas_validation_program_allowed(atlas_buf_cstr(&got[0].args[0])),
                "a fragment of an encoding was accepted as a program");

    atlas_orch_argv_free(&got[0]);
    atlas_buf_free(&wrapped);
    atlas_buf_free(&enc);
    atlas_orch_argv_free(&one);
}

/* --- 3: the same wrapper fails outright on a one-word gate ---------------- */

static void test_the_damage_depended_on_the_gate(void) {
    atlas_err err;
    atlas_err_init(&err);

    atlas_orch_argv one;
    atlas_orch_argv_init(&one);
    T_OK(atlas_orch_argv_push(&one, "make", 4u, &err), &err);
    atlas_buf enc = ATLAS_BUF_INIT;
    T_OK(atlas_orch_validations_encode(&one, 1u, &enc, &err), &err);
    T_CHECK(strcmp(atlas_buf_cstr(&enc), "1:1:4:make,") == 0);

    /* Decoded as the daemon now decodes it, `make` arrives as `make`. */
    atlas_orch_argv got[1];
    atlas_orch_argv_init(&got[0]);
    T_OK(atlas_orch_validation_wire_decode(atlas_buf_cstr(&enc), &got[0], &err), &err);
    T_EQ_INT((int)got[0].count, 1);
    T_CHECK(strcmp(atlas_buf_cstr(&got[0].args[0]), "make") == 0);
    T_CHECK(atlas_validation_program_allowed(atlas_buf_cstr(&got[0].args[0])));
    atlas_orch_argv_free(&got[0]);

    /* Wrapped, this one does not decode at all — the inner length runs into a
     * `:` where a `,` was required. So the defect had two faces: a gate whose
     * digits happened to line up was silently reduced to a fragment and stored,
     * and one whose digits did not was refused as malformed. Neither ever ran,
     * and the two symptoms look like unrelated bugs from the outside, which is
     * why the encoding — not the symptom — is what this suite asserts. */
    atlas_buf wrapped = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&wrapped, &err, "1:%s", atlas_buf_cstr(&enc)), &err);
    atlas_orch_argv bad[1];
    atlas_orch_argv_init(&bad[0]);
    atlas_err_init(&err);
    T_CHECK_MSG(atlas_orch_validation_wire_decode(atlas_buf_cstr(&wrapped), &bad[0], &err) !=
                    ATLAS_OK,
                "the double-wrapped one-word gate decoded, which it must not");

    atlas_orch_argv_free(&bad[0]);
    atlas_buf_free(&wrapped);
    atlas_buf_free(&enc);
    atlas_orch_argv_free(&one);
}

static const atlas_test TESTS[] = {
    {"a_multi_word_gate_survives_the_wire", test_a_multi_word_gate_survives_the_wire},
    {"wrapping_the_element_again_destroys_it", test_wrapping_the_element_again_destroys_it},
    {"the_damage_depended_on_the_gate", test_the_damage_depended_on_the_gate},
};

ATLAS_TEST_MAIN("orch_validation_wire", TESTS)

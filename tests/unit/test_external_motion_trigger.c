/**
 * @file test_external_motion_trigger.c
 * @brief Layer 1 Unity tests for external_motion_trigger state transitions
 *
 * Verifies the state-machine logic introduced/fixed in PR #356:
 *
 *   unified_detection_notify_motion() sets ctx->external_motion_trigger
 *   atomically.  The if (is_keyframe) block in process_packet() then
 *   consumes the flag and drives the following transitions:
 *
 *     BUFFERING   + trigger==1  →  RECORDING      (motion start)
 *     RECORDING   + trigger==2  →  POST_BUFFER    (motion end)
 *     POST_BUFFER + trigger==1  →  RECORDING      (keep-alive — was
 *                                                   previously unreachable
 *                                                   due to a state-guard bug)
 *
 * Layer 1: pure logic, no FFmpeg / SQLite / lightnvr_lib dependency.
 * The consumption logic in consume_trigger() mirrors the if (is_keyframe)
 * block in src/video/unified_detection_thread.c : process_packet().
 */

#define _POSIX_C_SOURCE 200809L

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "unity.h"

/* --------------------------------------------------------------------------
 * Minimal inline definition of the state enum and a cut-down ctx struct.
 * We cannot include unified_detection_thread.h here without pulling in
 * the full FFmpeg / lightnvr_lib dependency chain (Layer 3), so we
 * reproduce only the fields that the external_motion_trigger logic touches.
 * -------------------------------------------------------------------------- */
typedef enum {
    UDT_STATE_INITIALIZING = 0,
    UDT_STATE_CONNECTING,
    UDT_STATE_BUFFERING,
    UDT_STATE_RECORDING,
    UDT_STATE_POST_BUFFER,
    UDT_STATE_RECONNECTING,
    UDT_STATE_STOPPING,
    UDT_STATE_STOPPED
} unified_detection_state_t;

typedef struct {
    atomic_int   state;
    atomic_int   external_motion_trigger;
    atomic_llong last_detection_time;
    atomic_llong post_buffer_end_time;
    bool         annotation_only;
    int          post_buffer_seconds;
} test_ctx_t;

/* --------------------------------------------------------------------------
 * Local stub for unified_detection_notify_motion().
 *
 * The real function acquires contexts_mutex and calls find_context_by_name()
 * before storing the atomic — neither of which is available at Layer 1.
 * The observable effect is identical: atomic_store with 1 or 2.
 * -------------------------------------------------------------------------- */
static void notify_motion(test_ctx_t *ctx, bool motion_active)
{
    atomic_store(&ctx->external_motion_trigger, motion_active ? 1 : 2);
}

/* --------------------------------------------------------------------------
 * Simulate the if (is_keyframe) block from process_packet().
 * Returns the state after consumption.
 * -------------------------------------------------------------------------- */
static unified_detection_state_t consume_trigger(test_ctx_t *ctx, time_t now)
{
    unified_detection_state_t current =
        (unified_detection_state_t)atomic_load(&ctx->state);

    int ext_trigger = atomic_exchange(&ctx->external_motion_trigger, 0);

    if (ext_trigger == 1) {
        atomic_store(&ctx->last_detection_time, (long long)now);
        if (!ctx->annotation_only) {
            if (current == UDT_STATE_BUFFERING) {
                /*
                 * Real code calls udt_start_recording() here; on success it
                 * sets UDT_STATE_RECORDING.  We advance directly because the
                 * recording machinery is not available at Layer 1.
                 */
                atomic_store(&ctx->state, UDT_STATE_RECORDING);
            } else if (current == UDT_STATE_POST_BUFFER) {
                /* Keep-alive: resume recording (fixed in PR #356). */
                atomic_store(&ctx->state, UDT_STATE_RECORDING);
            }
            /* UDT_STATE_RECORDING: last_detection_time refresh is sufficient */
        }
    } else if (ext_trigger == 2) {
        if (!ctx->annotation_only && current == UDT_STATE_RECORDING) {
            atomic_store(&ctx->post_buffer_end_time,
                         (long long)(now + ctx->post_buffer_seconds));
            atomic_store(&ctx->state, UDT_STATE_POST_BUFFER);
        }
    }

    return (unified_detection_state_t)atomic_load(&ctx->state);
}

/* --------------------------------------------------------------------------
 * Test fixture helpers
 * -------------------------------------------------------------------------- */
static test_ctx_t make_ctx(unified_detection_state_t initial_state)
{
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    atomic_init(&ctx.state,                   initial_state);
    atomic_init(&ctx.external_motion_trigger, 0);
    atomic_init(&ctx.last_detection_time,     0LL);
    atomic_init(&ctx.post_buffer_end_time,    0LL);
    ctx.annotation_only     = false;
    ctx.post_buffer_seconds = 30;
    return ctx;
}

void setUp(void)    {}
void tearDown(void) {}

/* =========================================================================
 * T1 — notify_motion() sets the atomic flag correctly
 * ========================================================================= */
void test_notify_active_sets_flag_to_1(void)
{
    test_ctx_t ctx = make_ctx(UDT_STATE_BUFFERING);
    notify_motion(&ctx, true);
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&ctx.external_motion_trigger));
}

void test_notify_ended_sets_flag_to_2(void)
{
    test_ctx_t ctx = make_ctx(UDT_STATE_BUFFERING);
    notify_motion(&ctx, false);
    TEST_ASSERT_EQUAL_INT(2, atomic_load(&ctx.external_motion_trigger));
}

/* =========================================================================
 * T2 — BUFFERING + trigger==1  →  RECORDING
 * ========================================================================= */
void test_buffering_active_trigger_starts_recording(void)
{
    test_ctx_t ctx = make_ctx(UDT_STATE_BUFFERING);
    notify_motion(&ctx, true);
    unified_detection_state_t s = consume_trigger(&ctx, time(NULL));
    TEST_ASSERT_EQUAL_INT(UDT_STATE_RECORDING, s);
}

void test_buffering_active_trigger_resets_flag(void)
{
    test_ctx_t ctx = make_ctx(UDT_STATE_BUFFERING);
    notify_motion(&ctx, true);
    consume_trigger(&ctx, time(NULL));
    TEST_ASSERT_EQUAL_INT(0, atomic_load(&ctx.external_motion_trigger));
}

/* =========================================================================
 * T3 — RECORDING + trigger==2  →  POST_BUFFER
 * ========================================================================= */
void test_recording_ended_trigger_enters_post_buffer(void)
{
    test_ctx_t ctx = make_ctx(UDT_STATE_RECORDING);
    notify_motion(&ctx, false);
    unified_detection_state_t s = consume_trigger(&ctx, time(NULL));
    TEST_ASSERT_EQUAL_INT(UDT_STATE_POST_BUFFER, s);
}

void test_recording_ended_trigger_resets_flag(void)
{
    test_ctx_t ctx = make_ctx(UDT_STATE_RECORDING);
    notify_motion(&ctx, false);
    consume_trigger(&ctx, time(NULL));
    TEST_ASSERT_EQUAL_INT(0, atomic_load(&ctx.external_motion_trigger));
}

/* =========================================================================
 * T4 — POST_BUFFER + trigger==1  →  RECORDING  (regression guard)
 *
 * Before PR #356 the atomic_exchange lived inside the
 * "is_keyframe && != POST_BUFFER" guard.  A keep-alive arriving while in
 * POST_BUFFER would be silently discarded: the flag was never consumed and
 * the recording stopped even though motion was still active.
 * ========================================================================= */
void test_post_buffer_keepalive_resumes_recording(void)
{
    test_ctx_t ctx = make_ctx(UDT_STATE_POST_BUFFER);
    notify_motion(&ctx, true);
    unified_detection_state_t s = consume_trigger(&ctx, time(NULL));
    TEST_ASSERT_EQUAL_INT(UDT_STATE_RECORDING, s);
}

void test_post_buffer_keepalive_resets_flag(void)
{
    test_ctx_t ctx = make_ctx(UDT_STATE_POST_BUFFER);
    notify_motion(&ctx, true);
    consume_trigger(&ctx, time(NULL));
    TEST_ASSERT_EQUAL_INT(0, atomic_load(&ctx.external_motion_trigger));
}

/* =========================================================================
 * T5 — RECORDING + trigger==1  →  stays RECORDING (keep-alive refreshes time)
 * ========================================================================= */
void test_recording_keepalive_stays_recording(void)
{
    test_ctx_t ctx = make_ctx(UDT_STATE_RECORDING);
    notify_motion(&ctx, true);
    unified_detection_state_t s = consume_trigger(&ctx, time(NULL));
    TEST_ASSERT_EQUAL_INT(UDT_STATE_RECORDING, s);
}

/* =========================================================================
 * T6 — No trigger set  →  state unchanged, flag stays 0
 * ========================================================================= */
void test_no_trigger_leaves_state_unchanged(void)
{
    test_ctx_t ctx = make_ctx(UDT_STATE_BUFFERING);
    unified_detection_state_t s = consume_trigger(&ctx, time(NULL));
    TEST_ASSERT_EQUAL_INT(UDT_STATE_BUFFERING, s);
    TEST_ASSERT_EQUAL_INT(0, atomic_load(&ctx.external_motion_trigger));
}

/* =========================================================================
 * T7 — annotation_only  →  trigger does NOT change state
 * ========================================================================= */
void test_annotation_only_ignores_active_trigger(void)
{
    test_ctx_t ctx = make_ctx(UDT_STATE_BUFFERING);
    ctx.annotation_only = true;
    notify_motion(&ctx, true);
    unified_detection_state_t s = consume_trigger(&ctx, time(NULL));
    TEST_ASSERT_EQUAL_INT(UDT_STATE_BUFFERING, s);
}

void test_annotation_only_ignores_ended_trigger(void)
{
    test_ctx_t ctx = make_ctx(UDT_STATE_RECORDING);
    ctx.annotation_only = true;
    notify_motion(&ctx, false);
    unified_detection_state_t s = consume_trigger(&ctx, time(NULL));
    TEST_ASSERT_EQUAL_INT(UDT_STATE_RECORDING, s);
}

/* =========================================================================
 * T8 — ended trigger in BUFFERING  →  no-op
 * ========================================================================= */
void test_ended_trigger_in_buffering_is_noop(void)
{
    test_ctx_t ctx = make_ctx(UDT_STATE_BUFFERING);
    notify_motion(&ctx, false);
    unified_detection_state_t s = consume_trigger(&ctx, time(NULL));
    TEST_ASSERT_EQUAL_INT(UDT_STATE_BUFFERING, s);
}

/* =========================================================================
 * main
 * ========================================================================= */
int main(void)
{
    UNITY_BEGIN();

    /* T1 — flag setting */
    RUN_TEST(test_notify_active_sets_flag_to_1);
    RUN_TEST(test_notify_ended_sets_flag_to_2);

    /* T2 — BUFFERING → RECORDING */
    RUN_TEST(test_buffering_active_trigger_starts_recording);
    RUN_TEST(test_buffering_active_trigger_resets_flag);

    /* T3 — RECORDING → POST_BUFFER */
    RUN_TEST(test_recording_ended_trigger_enters_post_buffer);
    RUN_TEST(test_recording_ended_trigger_resets_flag);

    /* T4 — POST_BUFFER → RECORDING (regression guard) */
    RUN_TEST(test_post_buffer_keepalive_resumes_recording);
    RUN_TEST(test_post_buffer_keepalive_resets_flag);

    /* T5 — RECORDING keep-alive */
    RUN_TEST(test_recording_keepalive_stays_recording);

    /* T6 — no trigger */
    RUN_TEST(test_no_trigger_leaves_state_unchanged);

    /* T7 — annotation_only */
    RUN_TEST(test_annotation_only_ignores_active_trigger);
    RUN_TEST(test_annotation_only_ignores_ended_trigger);

    /* T8 — ended in BUFFERING */
    RUN_TEST(test_ended_trigger_in_buffering_is_noop);

    return UNITY_END();
}

#include "test_util.h"
#include "resolver.h"
#include "engine.h"

// ---------------------------------------------------------------------------
// Mock probe function — just marks probe as completed without real system calls
// ---------------------------------------------------------------------------

static void mock_run_probe(session_context *ctx, probe_key key) {
    // Simply mark the probe as done; the test pre-sets the context fields
    ctx->probes_completed |= (1u << key);
}

// ---------------------------------------------------------------------------
// Mock engines
// ---------------------------------------------------------------------------

static engine_detect_result mock_accept_detect(session_context *ctx) {
    (void)ctx;
    return ENGINE_ACCEPT;
}

static bool mock_accept_render(const notification *n,
                               const session_context *ctx) {
    (void)n; (void)ctx;
    return true;
}

static engine_detect_result mock_reject_detect(session_context *ctx) {
    (void)ctx;
    return ENGINE_REJECT;
}

static engine_detect_result mock_probe_then_accept_detect(session_context *ctx) {
    if (!context_probe_done(ctx, PROBE_HAS_DBUS_NOTIFICATIONS)) {
        ctx->requested_probe = PROBE_HAS_DBUS_NOTIFICATIONS;
        return ENGINE_NEED_PROBE;
    }
    if (ctx->has_dbus_notifications)
        return ENGINE_ACCEPT;
    return ENGINE_REJECT;
}

static engine_detect_result mock_probe_then_reject_detect(session_context *ctx) {
    if (!context_probe_done(ctx, PROBE_HAS_FRAMEBUFFER)) {
        ctx->requested_probe = PROBE_HAS_FRAMEBUFFER;
        return ENGINE_NEED_PROBE;
    }
    return ENGINE_REJECT;
}

static void mock_dismiss(void) {}

// ---------------------------------------------------------------------------
// Test suite
// ---------------------------------------------------------------------------

void test_resolver_suite(void) {

    // Test: first engine accepts
    {
        engine engines[] = {
            {"accept", 0, mock_accept_detect, mock_accept_render, mock_dismiss},
        };
        session_context ctx = {0};
        engine *selected = resolver_select(engines, 1, &ctx, mock_run_probe);
        ASSERT_NOT_NULL(selected, "first-accept: engine selected");
        ASSERT_STR_EQ(selected->name, "accept", "first-accept: correct engine");
    }

    // Test: skip rejected, pick second
    {
        engine engines[] = {
            {"reject", 0, mock_reject_detect, NULL, mock_dismiss},
            {"accept", 1, mock_accept_detect, mock_accept_render, mock_dismiss},
        };
        session_context ctx = {0};
        engine *selected = resolver_select(engines, 2, &ctx, mock_run_probe);
        ASSERT_NOT_NULL(selected, "skip-reject: engine selected");
        ASSERT_STR_EQ(selected->name, "accept",
                      "skip-reject: second engine picked");
    }

    // Test: all reject -> NULL
    {
        engine engines[] = {
            {"r1", 0, mock_reject_detect, NULL, mock_dismiss},
            {"r2", 1, mock_reject_detect, NULL, mock_dismiss},
        };
        session_context ctx = {0};
        engine *selected = resolver_select(engines, 2, &ctx, mock_run_probe);
        ASSERT_NULL(selected, "all-reject: no engine selected");
    }

    // Test: NEED_PROBE triggers probe, then re-evaluates
    {
        engine engines[] = {
            {"probe-accept", 0, mock_probe_then_accept_detect,
             mock_accept_render, mock_dismiss},
        };
        session_context ctx = {0};
        ctx.has_dbus_notifications = true;  // probe will "find" this
        engine *selected = resolver_select(engines, 1, &ctx, mock_run_probe);
        ASSERT_NOT_NULL(selected, "probe-accept: engine selected after probe");
        ASSERT_STR_EQ(selected->name, "probe-accept",
                      "probe-accept: correct engine");
        ASSERT_TRUE(context_probe_done(&ctx, PROBE_HAS_DBUS_NOTIFICATIONS),
                    "probe-accept: probe was marked completed");
    }

    // Test: NEED_PROBE, probe negative -> reject, fallback to next
    {
        engine engines[] = {
            {"probe-accept", 0, mock_probe_then_accept_detect,
             mock_accept_render, mock_dismiss},
            {"fallback", 1, mock_accept_detect, mock_accept_render, mock_dismiss},
        };
        session_context ctx = {0};
        ctx.has_dbus_notifications = false;
        engine *selected = resolver_select(engines, 2, &ctx, mock_run_probe);
        ASSERT_NOT_NULL(selected, "probe-fallback: engine selected");
        ASSERT_STR_EQ(selected->name, "fallback",
                      "probe-fallback: fell through to fallback");
    }

    // Test: rejected engines skipped on re-evaluation after probe
    {
        engine engines[] = {
            {"reject-fast", 0, mock_reject_detect, NULL, mock_dismiss},
            {"probe-accept", 1, mock_probe_then_accept_detect,
             mock_accept_render, mock_dismiss},
        };
        session_context ctx = {0};
        ctx.has_dbus_notifications = true;
        engine *selected = resolver_select(engines, 2, &ctx, mock_run_probe);
        ASSERT_NOT_NULL(selected, "skip-rejected-after-probe: engine selected");
        ASSERT_STR_EQ(selected->name, "probe-accept",
                      "skip-rejected-after-probe: correct engine");
    }

    // Test: NEED_PROBE for already-completed probe -> reject
    {
        engine engines[] = {
            {"probe-accept", 0, mock_probe_then_accept_detect,
             mock_accept_render, mock_dismiss},
            {"fallback", 1, mock_accept_detect, mock_accept_render, mock_dismiss},
        };
        session_context ctx = {0};
        ctx.has_dbus_notifications = true;
        // Pre-mark the probe as done so engine sees it done on first call
        // and goes straight to checking the field -> ACCEPT
        ctx.probes_completed |= (1u << PROBE_HAS_DBUS_NOTIFICATIONS);
        engine *selected = resolver_select(engines, 2, &ctx, mock_run_probe);
        ASSERT_NOT_NULL(selected, "pre-probed: engine selected");
        ASSERT_STR_EQ(selected->name, "probe-accept",
                      "pre-probed: accepted without re-probe");
    }

    // Test: probe then reject with fallback
    {
        engine engines[] = {
            {"probe-reject", 0, mock_probe_then_reject_detect,
             NULL, mock_dismiss},
            {"fallback", 1, mock_accept_detect, mock_accept_render, mock_dismiss},
        };
        session_context ctx = {0};
        engine *selected = resolver_select(engines, 2, &ctx, mock_run_probe);
        ASSERT_NOT_NULL(selected, "probe-then-reject: engine selected");
        ASSERT_STR_EQ(selected->name, "fallback",
                      "probe-then-reject: fell through to fallback");
        ASSERT_TRUE(context_probe_done(&ctx, PROBE_HAS_FRAMEBUFFER),
                    "probe-then-reject: framebuffer probe completed");
    }

    // Test: empty engine list -> NULL
    {
        session_context ctx = {0};
        engine *selected = resolver_select(NULL, 0, &ctx, mock_run_probe);
        ASSERT_NULL(selected, "empty-list: no engine selected");
    }

    // Test: NULL probe_fn uses context_run_probe (just verify it doesn't crash)
    // This would call real probes so we only test the non-probe path
    {
        engine engines[] = {
            {"accept", 0, mock_accept_detect, mock_accept_render, mock_dismiss},
        };
        session_context ctx = {0};
        engine *selected = resolver_select(engines, 1, &ctx, NULL);
        ASSERT_NOT_NULL(selected, "null-probe-fn: engine selected");
        ASSERT_STR_EQ(selected->name, "accept",
                      "null-probe-fn: correct engine");
    }

    // Test: multiple probes on different engines
    {
        engine engines[] = {
            {"probe-reject", 0, mock_probe_then_reject_detect,
             NULL, mock_dismiss},
            {"probe-accept", 1, mock_probe_then_accept_detect,
             mock_accept_render, mock_dismiss},
        };
        session_context ctx = {0};
        ctx.has_dbus_notifications = true;
        engine *selected = resolver_select(engines, 2, &ctx, mock_run_probe);
        ASSERT_NOT_NULL(selected, "multi-probe: engine selected");
        ASSERT_STR_EQ(selected->name, "probe-accept",
                      "multi-probe: second probe engine accepted");
        ASSERT_TRUE(context_probe_done(&ctx, PROBE_HAS_FRAMEBUFFER),
                    "multi-probe: first probe completed");
        ASSERT_TRUE(context_probe_done(&ctx, PROBE_HAS_DBUS_NOTIFICATIONS),
                    "multi-probe: second probe completed");
    }
}
